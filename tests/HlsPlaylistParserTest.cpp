#include "core/HlsPlaylistParser.h"

#include <openssl/evp.h>
#include <QtTest/QtTest>

class HlsPlaylistParserTest final : public QObject {
    Q_OBJECT
private slots:
    void identifiesM3u8Header()
    {
        QVERIFY(qtidm::HlsPlaylistParser::isM3u8("#EXTM3U\n#EXTINF:10.0,\nsegment1.ts"));
        QVERIFY(qtidm::HlsPlaylistParser::isM3u8("\xef\xbb\xbf#EXTM3U\r\n#EXTINF:5.0,\nsegment.ts"));
        QVERIFY(qtidm::HlsPlaylistParser::isM3u8("  \n#EXTM3U\n"));
        QVERIFY(!qtidm::HlsPlaylistParser::isM3u8("<html><body>Not a playlist</body></html>"));
        QVERIFY(!qtidm::HlsPlaylistParser::isM3u8(""));
    }

    void parsesAttributesCorrectly()
    {
        const QString line = QStringLiteral("METHOD=AES-128,URI=\"https://example.com/key.bin\",IV=0x0123456789ABCDEF,BANDWIDTH=5000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1920x1080");
        const auto attrs = qtidm::HlsPlaylistParser::parseAttributes(line);

        QCOMPARE(attrs.value(QStringLiteral("METHOD")), QStringLiteral("AES-128"));
        QCOMPARE(attrs.value(QStringLiteral("URI")), QStringLiteral("https://example.com/key.bin"));
        QCOMPARE(attrs.value(QStringLiteral("IV")), QStringLiteral("0x0123456789ABCDEF"));
        QCOMPARE(attrs.value(QStringLiteral("BANDWIDTH")), QStringLiteral("5000000"));
        QCOMPARE(attrs.value(QStringLiteral("CODECS")), QStringLiteral("avc1.640028,mp4a.40.2"));
        QCOMPARE(attrs.value(QStringLiteral("RESOLUTION")), QStringLiteral("1920x1080"));
    }

    void parsesMasterPlaylist()
    {
        const QString master = QStringLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
            "360p.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,FRAME-RATE=29.970\n"
            "720p/index.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"avc1.640028,mp4a.40.2\"\n"
            "https://cdn.example.com/1080p/index.m3u8\n"
        );

        const QUrl base(QStringLiteral("https://media.example.com/stream/master.m3u8"));
        const auto parsed = qtidm::HlsPlaylistParser::parse(master, base);

        QVERIFY(parsed.isMaster);
        QCOMPARE(parsed.variants.size(), 3);

        QCOMPARE(parsed.variants[0].width, 640);
        QCOMPARE(parsed.variants[0].height, 360);
        QCOMPARE(parsed.variants[0].uri, QUrl(QStringLiteral("https://media.example.com/stream/360p.m3u8")));

        QCOMPARE(parsed.variants[1].width, 1280);
        QCOMPARE(parsed.variants[1].height, 720);
        QCOMPARE(parsed.variants[1].uri, QUrl(QStringLiteral("https://media.example.com/stream/720p/index.m3u8")));

        QCOMPARE(parsed.variants[2].width, 1920);
        QCOMPARE(parsed.variants[2].height, 1080);
        QCOMPARE(parsed.variants[2].uri, QUrl(QStringLiteral("https://cdn.example.com/1080p/index.m3u8")));

        const int best = qtidm::HlsPlaylistParser::selectBestVariant(parsed.variants);
        QCOMPARE(best, 2); // 1080p has highest resolution

        const int best720 = qtidm::HlsPlaylistParser::selectBestVariant(parsed.variants, 720);
        QCOMPARE(best720, 1); // Capped at 720p
    }

    void parsesMasterPlaylistWithAudioRenditions()
    {
        const QString master = QStringLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:4\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio-low\",NAME=\"English Low\",DEFAULT=NO,AUTOSELECT=YES,LANGUAGE=\"en\",URI=\"audio_low.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio-high\",NAME=\"English High\",DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"en\",URI=\"audio_high.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio-high\",NAME=\"Spanish High\",DEFAULT=NO,AUTOSELECT=NO,LANGUAGE=\"es\",URI=\"audio_es.m3u8\"\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=3000000,RESOLUTION=1920x1080,AUDIO=\"audio-high\"\n"
            "video_1080p.m3u8\n"
        );

        const QUrl base(QStringLiteral("https://media.example.com/stream/master.m3u8"));
        const auto parsed = qtidm::HlsPlaylistParser::parse(master, base);

        QVERIFY(parsed.isMaster);
        QCOMPARE(parsed.audioRenditions.size(), 3);
        QCOMPARE(parsed.variants.size(), 1);
        QCOMPARE(parsed.variants[0].audioGroup, QStringLiteral("audio-high"));

        // Default track in audio-high group
        const QUrl audioUrl = qtidm::HlsPlaylistParser::selectAudioRenditionUrl(parsed, parsed.variants[0].audioGroup);
        QCOMPARE(audioUrl, QUrl(QStringLiteral("https://media.example.com/stream/audio_high.m3u8")));

        // Autoselect track in audio-low group
        const QUrl lowAudioUrl = qtidm::HlsPlaylistParser::selectAudioRenditionUrl(parsed, QStringLiteral("audio-low"));
        QCOMPARE(lowAudioUrl, QUrl(QStringLiteral("https://media.example.com/stream/audio_low.m3u8")));

        // Unknown audio group
        const QUrl noneUrl = qtidm::HlsPlaylistParser::selectAudioRenditionUrl(parsed, QStringLiteral("audio-missing"));
        QVERIFY(noneUrl.isEmpty());
    }

    void parsesMediaPlaylistWithByteRangesAndInit()
    {
        const QString media = QStringLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:7\n"
            "#EXT-X-TARGETDURATION:6\n"
            "#EXT-X-MEDIA-SEQUENCE:100\n"
            "#EXT-X-PLAYLIST-TYPE:VOD\n"
            "#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"1024@0\"\n"
            "#EXTINF:6.006,\n"
            "#EXT-X-BYTERANGE:500000@1024\n"
            "stream.mp4\n"
            "#EXTINF:5.998,\n"
            "#EXT-X-BYTERANGE:450000\n"
            "stream.mp4\n"
            "#EXTINF:4.120,\n"
            "chunk3.m4s\n"
            "#EXT-X-ENDLIST\n"
        );

        const QUrl base(QStringLiteral("https://cdn.example.com/hls/playlist.m3u8"));
        const auto parsed = qtidm::HlsPlaylistParser::parse(media, base);

        QVERIFY(!parsed.isMaster);
        QVERIFY(!parsed.isLive); // VOD has ENDLIST
        QCOMPARE(parsed.targetDuration, 6);
        QCOMPARE(parsed.mediaSequence, 100);
        QCOMPARE(parsed.segments.size(), 3);

        // Segment 0
        QCOMPARE(parsed.segments[0].index, 100);
        QCOMPARE(parsed.segments[0].duration, 6.006);
        QCOMPARE(parsed.segments[0].byteRangeLength, 500000);
        QCOMPARE(parsed.segments[0].byteRangeOffset, 1024);
        QCOMPARE(parsed.segments[0].initSegment.uri, QUrl(QStringLiteral("https://cdn.example.com/hls/init.mp4")));
        QCOMPARE(parsed.segments[0].initSegment.byteRangeLength, 1024);

        // Segment 1 (continuous byte range: 1024 + 500000 = 501024)
        QCOMPARE(parsed.segments[1].index, 101);
        QCOMPARE(parsed.segments[1].byteRangeLength, 450000);
        QCOMPARE(parsed.segments[1].byteRangeOffset, 501024);

        // Segment 2 (regular segment)
        QCOMPARE(parsed.segments[2].index, 102);
        QCOMPARE(parsed.segments[2].uri, QUrl(QStringLiteral("https://cdn.example.com/hls/chunk3.m4s")));
        QCOMPARE(parsed.segments[2].byteRangeLength, -1);
    }

    void handlesAes128KeysAndCalculatesIv()
    {
        const QString media = QStringLiteral(
            "#EXTM3U\n"
            "#EXT-X-TARGETDURATION:10\n"
            "#EXT-X-MEDIA-SEQUENCE:5\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",IV=0x00000000000000000000000000000005\n"
            "#EXTINF:10.0,\n"
            "enc0.ts\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
            "#EXTINF:10.0,\n"
            "enc1.ts\n"
            "#EXT-X-ENDLIST\n"
        );

        const QUrl base(QStringLiteral("https://cdn.example.com/hls/playlist.m3u8"));
        const auto parsed = qtidm::HlsPlaylistParser::parse(media, base);

        QCOMPARE(parsed.segments.size(), 2);

        // Segment 0: explicit IV
        QCOMPARE(parsed.segments[0].key.method, QStringLiteral("AES-128"));
        QCOMPARE(parsed.segments[0].key.uri, QUrl(QStringLiteral("https://cdn.example.com/hls/key.bin")));
        QCOMPARE(parsed.segments[0].key.iv.size(), 16);
        QCOMPARE(static_cast<quint8>(parsed.segments[0].key.iv.at(15)), quint8(5));

        // Segment 1: computed sequence IV for sequence 6
        QCOMPARE(parsed.segments[1].key.method, QStringLiteral("AES-128"));
        QCOMPARE(parsed.segments[1].key.iv.size(), 16);
        QCOMPARE(static_cast<quint8>(parsed.segments[1].key.iv.at(15)), quint8(6));
    }

    void decryptsAes128Cbc()
    {
        const QByteArray key = "0123456789abcdef"; // 16 bytes
        const QByteArray iv  = "fedcba9876543210"; // 16 bytes
        const QByteArray plaintext = "Hello HLS encrypted world! This is a test chunk with MPEG-TS payload.";

        // Encrypt with OpenSSL
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        QVERIFY(ctx != nullptr);
        QCOMPARE(EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                                    reinterpret_cast<const unsigned char*>(key.constData()),
                                    reinterpret_cast<const unsigned char*>(iv.constData())), 1);

        QByteArray ciphertext;
        ciphertext.resize(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
        int outLen1 = 0;
        QCOMPARE(EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &outLen1,
                                   reinterpret_cast<const unsigned char*>(plaintext.constData()),
                                   plaintext.size()), 1);
        int outLen2 = 0;
        QCOMPARE(EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + outLen1, &outLen2), 1);
        ciphertext.resize(outLen1 + outLen2);
        EVP_CIPHER_CTX_free(ctx);

        // Decrypt using HlsPlaylistParser::decryptAes128Cbc
        const QByteArray decrypted = qtidm::HlsPlaylistParser::decryptAes128Cbc(ciphertext, key, iv);
        QCOMPARE(decrypted, plaintext);
    }
};

QTEST_MAIN(HlsPlaylistParserTest)
#include "HlsPlaylistParserTest.moc"

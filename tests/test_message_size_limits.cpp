// The composer's and the send path's size check against the ceilings the
// server publishes in /versions (bsfchat.limits). The check is what turns an
// oversize send into a readable "Not sent" reason instead of a bounce.
#include <QTest>

#include "net/MessageSizeLimit.h"

using namespace bsfchat::net::msglimit;
using nlohmann::json;

class TestMessageSizeLimits : public QObject {
    Q_OBJECT

private slots:
    void publishedValueIsUsed()
    {
        json lim = {{"max_message_bytes", 4096}};
        QCOMPARE(readLimit(lim, "max_message_bytes", 16384), 4096);
    }

    void missingOrUnusableValueKeepsTheDefault()
    {
        QCOMPARE(readLimit(json::object(), "max_message_bytes", 16384), 16384);
        QCOMPARE(readLimit(json({{"max_message_bytes", 0}}), "max_message_bytes", 16384), 16384);
        QCOMPARE(readLimit(json({{"max_message_bytes", "4096"}}), "max_message_bytes", 16384),
                 16384);
        QCOMPARE(readLimit(json({{"max_message_bytes", -1}}), "max_message_bytes", 16384), 16384);
        QCOMPARE(readLimit(json(), "max_message_bytes", 16384), 16384);
    }

    void bodyAtTheLimitIsAccepted()
    {
        const json msg = {{"msgtype", "m.text"}, {"body", std::string(16384, 'a')}};
        QVERIFY(messageSizeProblem(msg, 16384, 65536).isEmpty());
    }

    void bodyOneByteOverIsRefusedWithReason()
    {
        const json msg = {{"msgtype", "m.text"}, {"body", std::string(16385, 'a')}};
        const QString why = messageSizeProblem(msg, 16384, 65536);
        QVERIFY(why.contains(QStringLiteral("16385 bytes")) || why.contains(QStringLiteral("16,385 bytes")));
        QVERIFY(why.contains(QStringLiteral("16384 bytes")) || why.contains(QStringLiteral("16,384 bytes")));
        QVERIFY(why.contains(QStringLiteral("this server accepts up to")));
    }

    void sizeIsCountedInBytesNotCharacters()
    {
        // 4,000 four-byte characters is 16,000 bytes, but a QString of 4,000
        // code points would look far shorter. Under a 16,000-byte limit it is
        // refused; the byte count is what the server checks.
        std::string four_byte;
        for (int i = 0; i < 4000; ++i) four_byte += "\xF0\x9F\x98\x80"; // U+1F600
        const json msg = {{"body", four_byte}};
        // Exactly 16,000 bytes: accepted at a 16,000-byte limit, refused one
        // byte below it. A code-point count (4,000) would have fit under both.
        QVERIFY(messageSizeProblem(msg, 16000, 64000).isEmpty());
        QVERIFY(!messageSizeProblem(msg, 15999, 64000).isEmpty());
    }

    void formattedBodyHasItsOwnCeiling()
    {
        // A short plain body whose markup blows past the formatted ceiling is
        // still refused, and the reason says it is the formatting.
        const json msg = {{"body", "hi"}, {"formatted_body", std::string(70000, 'x')}};
        const QString why = messageSizeProblem(msg, 16384, 65536);
        QVERIFY(!why.isEmpty());
        QVERIFY(why.contains(QStringLiteral("formatting")));
    }

    void plainMessageWithNoBodyFieldFits()
    {
        QVERIFY(messageSizeProblem(json::object(), 16384, 65536).isEmpty());
    }

    void shortMessageIsSentInline()
    {
        const json msg = {{"msgtype", "m.text"}, {"body", "hello"}};
        const auto d = planSend(msg, 16384, 65536, 50 * 1024 * 1024);
        QCOMPARE(int(d.plan), int(SendPlan::Inline));
    }

    void oversizeMarkupDropsFormattingNotTheText()
    {
        // The text fits; the markup does not. The text must still arrive.
        const json msg = {{"msgtype", "m.text"}, {"body", "hi"},
                          {"format", "org.matrix.custom.html"},
                          {"formatted_body", std::string(70000, 'x')}};
        const auto d = planSend(msg, 16384, 65536, 50 * 1024 * 1024);
        QCOMPARE(int(d.plan), int(SendPlan::InlineWithoutFormatting));
    }

    void longTextBecomesAFile()
    {
        const json msg = {{"msgtype", "m.text"}, {"body", std::string(40000, 'a')}};
        const auto d = planSend(msg, 16384, 65536, 50 * 1024 * 1024);
        QCOMPARE(int(d.plan), int(SendPlan::AsFile));
    }

    void longTextOverTheUploadLimitIsRefusedWithAReason()
    {
        const json msg = {{"msgtype", "m.text"}, {"body", std::string(2 * 1024 * 1024, 'a')}};
        const auto d = planSend(msg, 16384, 65536, 1024 * 1024);
        QCOMPARE(int(d.plan), int(SendPlan::Refuse));
        QVERIFY(d.reason.contains(QStringLiteral("as a file")));
    }

    void emotesAreNeverTurnedIntoFiles()
    {
        // /me is an m.emote; as a file it would stop being an emote.
        const json msg = {{"msgtype", "m.emote"}, {"body", std::string(40000, 'a')}};
        const auto d = planSend(msg, 16384, 65536, 50 * 1024 * 1024);
        QCOMPARE(int(d.plan), int(SendPlan::Refuse));
        QVERIFY(!d.reason.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestMessageSizeLimits)
#include "test_message_size_limits.moc"

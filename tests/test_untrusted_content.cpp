// Other users' input must never crash the client or reach the OS unchecked.
//
//   * C-1: a timeline event whose content has the wrong JSON types
//     (`"formatted_body": 123`) threw nlohmann::type_error out of the model —
//     out of a Q_INVOKABLE on the room switch, terminating the app, and again
//     on every launch from the cache.
//   * V-2: a call event's `bsfchat_caps` with `"video_rtp": "1"` threw out of
//     PeerCaps::fromJson for every participant.
//   * M-1: the attachment cache file took its extension from the sender's mxc
//     path, and opening it handed `x.bat` to ShellExecute.
//   * Q-S1: any href scheme went to the OS, and link text could name a
//     different site than the link went to.

#include "core/AttachmentPolicy.h"
#include "model/MessageModel.h"
#include "util/JsonSafe.h"
#include "util/LinkPolicy.h"
#include "util/MediaUrl.h"
#include "voice/PeerCaps.h"

#include <QTest>

#include <bsfchat/Constants.h>

namespace js = bsfchat::jsonsafe;
namespace attachment = bsfchat::client::attachment;
namespace links = bsfchat::client::links;

namespace {

bsfchat::RoomEvent messageWith(const std::string& id, nlohmann::json content,
                               const std::string& type = std::string(bsfchat::event_type::kRoomMessage))
{
    bsfchat::RoomEvent ev;
    ev.event_id = id;
    ev.sender = "@mallory:server";
    ev.type = type;
    ev.origin_server_ts = 1000;
    ev.content.data = std::move(content);
    return ev;
}

} // namespace

class TestUntrustedContent : public QObject {
    Q_OBJECT

private slots:
    void jsonReadsNeverThrow()
    {
        const auto j = nlohmann::json::parse(
            R"({"s":"x","n":5,"f":2.9,"neg":-3,"big":18446744073709551615,
                "b":true,"one":1,"arr":[1],"obj":{"k":"v"},"nul":null})");
        QCOMPARE(js::str(j, "s"), std::string("x"));
        QCOMPARE(js::str(j, "n"), std::string());
        QCOMPARE(js::str(j, "missing", "d"), std::string("d"));
        QCOMPARE(js::qstr(j, "obj"), QString());
        QCOMPARE(js::integer(j, "n"), qint64(5));
        QCOMPARE(js::integer(j, "f"), qint64(2));
        QCOMPARE(js::integer(j, "s", 7), qint64(7));
        QCOMPARE(js::integer(j, "big"), std::numeric_limits<qint64>::max());
        QCOMPARE(js::int32(j, "big"), std::numeric_limits<int>::max());
        QCOMPARE(js::count(j, "neg"), quint64(0));
        QVERIFY(js::boolean(j, "b"));
        QVERIFY(js::boolean(j, "one"));
        QVERIFY(!js::boolean(j, "s"));
        QVERIFY(js::boolean(j, "nul", true));
        QVERIFY(js::object(j, "arr").is_object());
        QVERIFY(js::array(j, "obj").is_array());
        QCOMPARE(js::str(js::object(j, "obj"), "k"), std::string("v"));
        // Not an object at all.
        const nlohmann::json notObj = "string";
        QCOMPARE(js::str(notObj, "k"), std::string());
        QVERIFY(js::member(nlohmann::json::array(), "k") == nullptr);
    }

    void aMalformedMessageIsRenderedOrSkippedNeverThrown()
    {
        MessageModel model;
        const auto bad = nlohmann::json::parse(R"({
            "msgtype": 7, "body": ["x"], "formatted_body": 123,
            "url": {"x": 1}, "info": {"size": "big", "w": "1", "h": null, "mimetype": 5},
            "m.relates_to": {"rel_type": 9, "event_id": 1,
                             "m.in_reply_to": {"event_id": false}},
            "m.mentions": {"user_ids": "@a:b", "room": "yes"}
        })");
        model.appendEvent(messageWith("$bad", bad), "@me:server");
        model.appendEvent(messageWith("$str", nlohmann::json("not an object")), "@me:server");
        model.appendEvent(messageWith("$ok", {{"msgtype", "m.text"}, {"body", "fine"}}),
                          "@me:server");
        QVERIFY(model.rowCount() >= 1);
        bool sawOk = false;
        for (int r = 0; r < model.rowCount(); ++r)
            sawOk |= model.data(model.index(r), MessageModel::BodyRole).toString() == "fine";
        QVERIFY(sawOk);
    }

    void malformedRelationsDoNotThrow()
    {
        MessageModel model;
        model.appendEvent(messageWith("$t", {{"msgtype", "m.text"}, {"body", "t"}}), "@me:s");
        // Edit with a non-string new body, reaction with a numeric key,
        // redaction with an object target.
        model.appendEvent(messageWith("$e", nlohmann::json::parse(R"({
            "msgtype":"m.text","body":"* x",
            "m.relates_to":{"rel_type":"m.replace","event_id":"$t"},
            "m.new_content":{"body":5,"formatted_body":{"a":1}}})")), "@me:s");
        model.appendEvent(messageWith("$r", nlohmann::json::parse(
            R"({"m.relates_to":{"rel_type":"m.annotation","event_id":"$t","key":3}})"),
            "m.reaction"), "@me:s");
        model.appendEvent(messageWith("$x", nlohmann::json::parse(R"({"redacts":{"id":1}})"),
                                      std::string(bsfchat::event_type::kRoomRedaction)), "@me:s");
        QVector<bsfchat::RoomEvent> window{
            messageWith("$w1", nlohmann::json::parse(R"({"msgtype":"m.text","formatted_body":1})")),
            messageWith("$w2", {{"msgtype", "m.text"}, {"body", "after"}}),
        };
        model.ingestCachedWindow(window, "@me:s");
        model.prependEvents(window, "@me:s");
        QVERIFY(model.rowCount() >= 2);
    }

    void aSendersFormattedBodyIsSanitisedOnTheWayIn()
    {
        MessageModel model;
        model.appendEvent(messageWith("$h", {{"msgtype", "m.text"}, {"body", "hi"},
                                             {"format", "org.matrix.custom.html"},
                                             {"formatted_body",
                                              "<b>hi</b><img src=\"https://t/p.png\">"
                                              "<a href=\"file:///c:/x.exe\">x</a>"}}),
                          "@me:s");
        const QString html =
            model.data(model.index(0), MessageModel::FormattedBodyRole).toString();
        QVERIFY(html.contains("<b>hi</b>"));
        QVERIFY(!html.contains("<img"));
        QVERIFY(!html.contains("file:"));
    }

    void peerCapsOfTheWrongTypeAreIgnored()
    {
        const auto caps = PeerCaps::fromJson(nlohmann::json::parse(
            R"({"video_rtp":"1","control_dc":true,"video_codecs":"h264",
                "h264_profiles_decode":[1,"cb"],"lossless":{"x":1}})"));
        QVERIFY(!caps.videoRtp);
        QVERIFY(caps.controlDc);
        QVERIFY(caps.videoCodecs.isEmpty());
        QCOMPARE(caps.h264ProfilesDecode, QStringList{"cb"});
        QVERIFY(caps.lossless.isEmpty());
        QVERIFY(PeerCaps::fromJson(nlohmann::json::parse(R"({"video_rtp":1})")).videoRtp);
        QVERIFY(!PeerCaps::fromJson(nlohmann::json(42)).videoRtp);
    }

    void mxcUrisAreStrict()
    {
        using bsfchat::client::isValidMxcUri;
        QVERIFY(isValidMxcUri("mxc://bsfchat.example/AbC_d-9"));
        QVERIFY(isValidMxcUri("mxc://host:8448/abc"));
        QVERIFY(isValidMxcUri("mxc://[::1]:8448/abc"));
        QVERIFY(isValidMxcUri("mxc://s/legacy.id")); // the server accepts dotted legacy ids
        for (const char* bad : {"mxc://s/id/evil.bat", "mxc://s/id#.bat", "mxc://s/id?x=1",
                                "mxc://s/.hidden", "mxc://s/", "mxc:///id", "mxc://s",
                                "mxc://s/i%2Fd", "mxc://s/../x", "mxc://s/id ", "http://s/id",
                                "mxc://us@er/id", "mxc://s/\u00e9"}) {
            QVERIFY2(!isValidMxcUri(QString::fromUtf8(bad)), bad);
            QVERIFY(bsfchat::client::buildMediaDownloadUrl("https://h", QString::fromUtf8(bad))
                        .isEmpty());
        }
    }

    void attachmentExtensionsComeFromTheAllowlistOnly()
    {
        QCOMPARE(attachment::cacheExtension("cat.PNG", ""), QStringLiteral(".png"));
        QCOMPARE(attachment::cacheExtension("report", "application/pdf"), QStringLiteral(".pdf"));
        QCOMPARE(attachment::cacheExtension("run.bat", ""), QString());
        QCOMPARE(attachment::cacheExtension("run.bat", "image/png"), QStringLiteral(".png"));
        QCOMPARE(attachment::cacheExtension("page.html", "text/html"), QString());
        QCOMPARE(attachment::cacheExtension("", ""), QString());
    }

    void programsAndScriptsAreRefused()
    {
        using K = attachment::Kind;
        for (const char* name : {"setup.exe", "x.MSI", "a.bat", "a.cmd", "a.scr", "a.ps1",
                                 "a.vbs", "a.js", "a.jse", "a.wsf", "a.hta", "a.jar", "a.lnk",
                                 "a.reg", "a.cpl", "Foo.app", "a.command", "a.sh", "a.pkg",
                                 "a.dmg", "a.apk", "invoice.pdf.exe", "invoice.exe.",
                                 "invoice.exe  ", "dir\\x.com", "a.exe:stream", "a.svg",
                                 "a.html"}) {
            QVERIFY2(attachment::classify(QString::fromUtf8(name), "") == K::Blocked, name);
        }
        QCOMPARE(attachment::classify("photo.jpg", "application/x-msdownload"), K::Blocked);
        QCOMPARE(attachment::classify("photo", "application/vnd.android.package-archive"),
                 K::Blocked);
        QCOMPARE(attachment::classify("photo.jpg", "image/jpeg"), K::Media);
        QCOMPARE(attachment::classify("clip", "video/mp4"), K::Media);
        QCOMPARE(attachment::classify("doc.pdf", "application/pdf"), K::Document);
        QCOMPARE(attachment::classify("invoice.exe.pdf", ""), K::Document);
        QCOMPARE(attachment::classify("", ""), K::Document);
    }

    void onlyWebAndMailLinksLeaveTheApp()
    {
        using A = links::Action;
        QCOMPARE(links::decide("https://example.com/x").action, A::Open);
        QCOMPARE(links::decide("HTTP://example.com").action, A::Open);
        QCOMPARE(links::decide("mailto:a@b.c").action, A::Open);
        QCOMPARE(links::decide("bsfchat://channel/x").action, A::Internal);
        for (const char* bad : {"file:///C:/Windows/calc.exe", "file://server/share/x",
                                "\\\\server\\share\\x", "smb://server/share", "javascript:alert(1)",
                                "ms-msdt:/id", "search-ms:query=x", "steam://run/1", "vbscript:x",
                                "data:text/html,x", "", "relative/path", "https://",
                                "http://exa\nmple.com"}) {
            QVERIFY2(links::decide(QString::fromUtf8(bad)).action == A::Refuse, bad);
        }
    }

    void aLinkWhoseTextNamesAnotherSiteNeedsConfirmation()
    {
        using A = links::Action;
        const QString href = "https://evil.example/login";
        const QString html = "<a href=\"https://evil.example/login\" style=\"x\">"
                             "https://bank.example/login</a> and "
                             "<a href=\"https://ok.example/\">click here</a>";
        QCOMPARE(links::anchorTextsFor(html, href), QStringList{"https://bank.example/login"});
        const auto d = links::decide(href, links::anchorTextsFor(html, href));
        QCOMPARE(d.action, A::Confirm);
        QCOMPARE(d.host, QStringLiteral("evil.example"));
        // Same host (www. and case aside) is fine, and so is prose.
        QCOMPARE(links::decide("https://www.Example.com/a", {"example.com/a"}).action, A::Open);
        QCOMPARE(links::decide("https://ok.example/", {"click here"}).action, A::Open);
        QCOMPARE(links::decide("https://ok.example/", {"bank.example"}).action, A::Confirm);
        // Entities in our own markup are compared decoded.
        QCOMPARE(links::anchorTextsFor("<a href=\"https://x/?a=1&amp;b=2\">x</a>",
                                       "https://x/?a=1&b=2"),
                 QStringList{"x"});
    }
};

QTEST_GUILESS_MAIN(TestUntrustedContent)
#include "test_untrusted_content.moc"

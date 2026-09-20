// Rendered line counts for plain, multi-line message bodies.
//
// The bug this guards: a body that arrives with no `formatted_body` is plain
// text, and the timeline shows it verbatim — newlines and all. The moment
// anything promotes that body to HTML (the " (edited)" badge, the m.emote
// prefix, the mention renderer), the newlines have to become <br> or the HTML
// importer folds them into spaces. They didn't, so editing a three-line
// message re-rendered it as one line.
//
// The probe is QTextDocument, which IS the importer behind TextEdit.RichText:
// counting the lines it lays out is the only way to assert what the user sees
// without launching the GUI (which on a dev Mac sets off permission dialogs).
// Asserting on markup would just re-state the implementation.
#include <QTest>
#include <QFile>
#include <QString>
#include <QTextDocument>

#include "model/MessageModel.h"
#include "util/MarkdownParser.h"

#include <bsfchat/MatrixTypes.h>
#include <bsfchat/Constants.h>

class TestMessageRender : public QObject {
    Q_OBJECT

private:
    static bsfchat::RoomEvent makeMessage(const std::string& eventId,
                                          const std::string& body,
                                          const std::string& msgtype = "m.text",
                                          int64_t ts = 1000)
    {
        bsfchat::RoomEvent event;
        event.event_id = eventId;
        event.sender = "@alice:server";
        event.type = std::string(bsfchat::event_type::kRoomMessage);
        event.origin_server_ts = ts;
        event.content.data = {{"msgtype", msgtype}, {"body", body}};
        return event;
    }

    // The sibling m.replace event: what a live edit looks like while the user
    // is watching the room.
    static bsfchat::RoomEvent makeEdit(const std::string& eventId,
                                       const std::string& targetId,
                                       const std::string& newBody,
                                       const std::string& msgtype = "m.text")
    {
        auto event = makeMessage(eventId, "* " + newBody, msgtype, 5000);
        event.content.data["m.relates_to"] =
            {{"rel_type", "m.replace"}, {"event_id", targetId}};
        event.content.data["m.new_content"] =
            {{"msgtype", msgtype}, {"body", newBody}};
        return event;
    }

    // The server-reconciled shape: the same message already edited when the
    // client loads the room. A different code path from the live edit.
    static bsfchat::RoomEvent makeReconciled(const std::string& eventId,
                                             const std::string& currentBody,
                                             const std::string& originalBody,
                                             const std::string& msgtype = "m.text")
    {
        auto event = makeMessage(eventId, currentBody, msgtype, 1000);
        nlohmann::json unsignedData;
        unsignedData["m.relations"]["m.replace"] = {
            {"event_id", "$theedit"}, {"sender", "@alice:server"},
            {"origin_server_ts", 5000}};
        unsignedData["bsfchat.original_content"] =
            {{"msgtype", msgtype}, {"body", originalBody}};
        event.unsigned_data = bsfchat::EventContent{unsignedData};
        return event;
    }

    static bsfchat::RoomEvent withMention(bsfchat::RoomEvent event,
                                          const std::string& userId)
    {
        event.content.data["m.mentions"] =
            nlohmann::json{{"user_ids", nlohmann::json::array({userId})}};
        return event;
    }

    // --- the probe -----------------------------------------------------
    // QTextDocument lays <br> and block boundaries out as line separators;
    // toPlainText() turns both back into '\n'.
    static int renderedLines(const QString& html)
    {
        QTextDocument doc;
        doc.setHtml(html);
        return doc.toPlainText().split(QLatin1Char('\n')).size();
    }

    static int plainLines(const QString& body)
    {
        return body.split(QLatin1Char('\n')).size();
    }

    // Mirror of the `text:` / `textFormat:` expression on the body TextEdit in
    // qml/components/MessageBubble.qml. The view only ever CONCATENATES —
    // every byte of HTML for the body comes from the model, which is what
    // makes this reachable from a C++ probe at all. The bubble escaping a body
    // itself is the bug, and testBubbleQmlDoesNotEscapeBodiesItself() below
    // fails if it starts doing that again.
    static QString bubbleText(const QString& msgtype, const QString& body,
                              const QString& formattedBody, bool edited)
    {
        const QString badge =
            QStringLiteral("<span style=\"color:#8e9297;font-size:small\">"
                           " (edited)</span>");
        if (msgtype == QLatin1String("m.emote")) {
            QString out = QStringLiteral("<span>* Alice</span> <i>")
                + formattedBody + QStringLiteral("</i>");
            if (edited) out += badge;
            return out;
        }
        const QString base = formattedBody.isEmpty() ? body : formattedBody;
        if (!edited) return base;
        return base + badge;
    }

    static bool bubbleIsRich(const QString& msgtype, const QString& formattedBody,
                             bool edited)
    {
        return msgtype == QLatin1String("m.emote") || !formattedBody.isEmpty()
            || edited;
    }

    // What the user ends up looking at, for one row of the model.
    static int bubbleLines(const MessageModel& model, int row)
    {
        const QString msgtype =
            model.data(model.index(row), MessageModel::MsgtypeRole).toString();
        const QString body =
            model.data(model.index(row), MessageModel::BodyRole).toString();
        const QString formatted =
            model.data(model.index(row), MessageModel::FormattedBodyRole).toString();
        const bool edited =
            model.data(model.index(row), MessageModel::EditedRole).toBool();
        const QString text = bubbleText(msgtype, body, formatted, edited);
        return bubbleIsRich(msgtype, formatted, edited) ? renderedLines(text)
                                                        : plainLines(text);
    }

private slots:
    // The headline report: send three lines, fix a typo, watch the layout
    // survive. Every msgtype the bubble renders as prose, both edit paths.
    void testEditKeepsLineBreaks_data()
    {
        QTest::addColumn<QString>("msgtype");
        QTest::addColumn<QString>("body");
        for (const QString& t : {QStringLiteral("m.text"),
                                 QStringLiteral("m.notice"),
                                 QStringLiteral("m.emote")}) {
            QTest::newRow(qPrintable(t + "/three lines"))
                << t << QStringLiteral("line one\nline two\nline three");
            QTest::newRow(qPrintable(t + "/blank line between"))
                << t << QStringLiteral("para one\n\npara two");
            QTest::newRow(qPrintable(t + "/trailing newline"))
                << t << QStringLiteral("body\n");
            QTest::newRow(qPrintable(t + "/only newlines"))
                << t << QStringLiteral("\n\n");
        }
    }

    void testEditKeepsLineBreaks()
    {
        QFETCH(QString, msgtype);
        QFETCH(QString, body);
        const int expected = plainLines(body);
        const std::string mt = msgtype.toStdString();
        const std::string b = body.toStdString();

        // 1. As sent: plain body, no formatted_body, never edited.
        {
            MessageModel model;
            model.appendEvent(makeMessage("$a", b, mt), "@bob:server");
            QCOMPARE(bubbleLines(model, 0), expected);
        }
        // 2. Edited live, in front of the user: sibling m.replace event.
        {
            MessageModel model;
            model.appendEvent(makeMessage("$a", "typo", mt), "@bob:server");
            model.appendEvent(makeEdit("$e", "$a", b, mt), "@bob:server");
            QCOMPARE(bubbleLines(model, 0), expected);
        }
        // 3. Already edited when the room loaded: server-reconciled bundle.
        {
            MessageModel model;
            model.appendEvent(makeReconciled("$a", b, "typo", mt), "@bob:server");
            QCOMPARE(bubbleLines(model, 0), expected);
        }
        // 4. Mentions: applyMentionMarkup escapes a plain body itself, which
        //    promotes the row to RichText with no edit involved at all.
        {
            MessageModel model;
            model.appendEvent(withMention(makeMessage("$a", b, mt), "@bob:server"),
                              "@bob:server");
            QCOMPARE(bubbleLines(model, 0), expected);
        }
    }

    // The escape-only promotion, at the level it lives at: plain text in,
    // HTML out, same number of lines. Markdown's path already did this; this
    // is the one that didn't.
    void testPlainToHtmlPreservesLines_data()
    {
        QTest::addColumn<QString>("body");
        QTest::newRow("three lines") << QStringLiteral("one\ntwo\nthree");
        QTest::newRow("blank line") << QStringLiteral("one\n\ntwo");
        QTest::newRow("trailing") << QStringLiteral("one\n");
        QTest::newRow("leading") << QStringLiteral("\none");
        QTest::newRow("only newlines") << QStringLiteral("\n\n\n");
        QTest::newRow("single line") << QStringLiteral("just one");
        // Markup in the body stays literal — escaping is the whole point of
        // this path, and <br> must not give a `<b>` an opening.
        QTest::newRow("angle brackets") << QStringLiteral("<b>one</b>\ntwo");
    }

    void testPlainToHtmlPreservesLines()
    {
        QFETCH(QString, body);
        QCOMPARE(renderedLines(MarkdownParser::plainToHtml(body)),
                 plainLines(body));
    }

    void testPlainToHtmlEscapesMarkup()
    {
        const QString out = MarkdownParser::plainToHtml(
            QStringLiteral("<b>bold</b> & *stars*\nnext"));
        QVERIFY(!out.contains(QLatin1String("<b>")));
        QVERIFY(out.contains(QLatin1String("<br>")));
        // Not markdown: a literal asterisk must survive an escape-only
        // promotion as a literal asterisk.
        QCOMPARE(renderedLines(out), 2);
        QTextDocument doc;
        doc.setHtml(out);
        QCOMPARE(doc.toPlainText(), QStringLiteral("<b>bold</b> & *stars*\nnext"));
    }

    // Markdown's own path was already correct; pin it so the two promotions
    // cannot drift apart again.
    void testMarkdownPathPreservesLines()
    {
        QCOMPARE(renderedLines(MarkdownParser::toHtml(
                     QStringLiteral("one\ntwo\nthree"))), 3);
        QCOMPARE(renderedLines(MarkdownParser::toHtml(
                     QStringLiteral("**bold**\nplain"))), 2);
    }

    // The composition mirrored in bubbleText() above lives in QML, out of
    // reach of QTextDocument. What keeps the mirror honest is that the bubble
    // has no HTML of its own to get wrong: it may escape the sender's display
    // name (one line, no breaks possible) and nothing else. An escape of
    // `bubble.body` in there is the bug coming back — that is the promotion
    // that dropped the newlines, and it is invisible to every C++ test.
    void testBubbleQmlDoesNotEscapeBodiesItself()
    {
        QFile f(QStringLiteral(BSFCHAT_QML_DIR "/components/MessageBubble.qml"));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text),
                 "MessageBubble.qml unreadable");
        const QString src = QString::fromUtf8(f.readAll());
        const int escapes = src.count(QLatin1String("replace(/&/g, \"&amp;\")"));
        QVERIFY2(escapes <= 1,
                 qPrintable(QStringLiteral(
                     "MessageBubble.qml builds HTML from a body itself (%1 "
                     "escape sites; only the sender display name may be "
                     "escaped there). Plain bodies become HTML in "
                     "MessageModel, where the line breaks are tested.")
                         .arg(escapes)));
    }
};

QTEST_MAIN(TestMessageRender)
#include "test_message_render.moc"

// Reading string fields out of a profile reply body.
//
// The bug this pins: a server answered the literal JSON `null` for an account
// with no display name and no picture — the default state of every fresh
// registration — and the client read it with `value(key, "")` inside an empty
// `catch (...)`. nlohmann throws type_error.306 on a null there, the catch ate
// it, and profileResult was never emitted, so every caller waited forever for
// an answer that could not arrive and nothing appeared in the log.
//
// So the two properties worth holding are independent, and both are asserted
// for each shape below: the read always produces usable values, AND a body
// that was not fully understood always leaves something to log.
//
// Pure logic, no network and no GUI app: MatrixClient reaches this through one
// free function precisely so the failure modes can be exercised without a
// server willing to reproduce them.

#include "net/ReplyFields.h"

#include <QTest>

// Bodies carrying an mxc:// URI are written as escaped literals rather than
// R"(...)": moc 6.10.2 treats the "//" inside a raw string as the start of a
// comment and swallows the rest of the file, so the whole class silently
// disappears and the test binary fails to link. Do not "tidy" these back.

using bsfchat::client::readStringFields;

namespace {

bsfchat::client::StringFields profile(const char* body)
{
    return readStringFields(body, {"displayname", "avatar_url"});
}

} // namespace

class TestReplyFields : public QObject {
    Q_OBJECT
private slots:

    // --- The happy path ------------------------------------------------

    void readsBothFields()
    {
        const auto f = profile("{\"displayname\":\"Ada\",\"avatar_url\":\"mxc://h/a\"}");
        QCOMPARE(f.values.at(0), QStringLiteral("Ada"));
        QCOMPARE(f.values.at(1), QStringLiteral("mxc://h/a"));
        QVERIFY(f.warning.isEmpty());
    }

    void absentKeysAreEmptyAndNotAnError()
    {
        // A profile with neither field set is ordinary, not a fault: an empty
        // object is the correct answer for a bare account and must not warn.
        const auto f = profile("{}");
        QCOMPARE(f.values.at(0), QString());
        QCOMPARE(f.values.at(1), QString());
        QVERIFY(f.warning.isEmpty());
    }

    void extraKeysAreIgnored()
    {
        const auto f = profile(R"({"displayname":"Ada","nickname":"a","x":1})");
        QCOMPARE(f.values.at(0), QStringLiteral("Ada"));
        QCOMPARE(f.values.at(1), QString());
        QVERIFY(f.warning.isEmpty());
    }

    // --- The regression ------------------------------------------------

    void nullBodyYieldsEmptyFieldsAndWarns()
    {
        // The exact body the server sent. Before the fix this threw
        // type_error.306 out of value() and took the whole reply with it.
        const auto f = profile("null");
        QCOMPARE(f.values.size(), 2);
        QCOMPARE(f.values.at(0), QString());
        QCOMPARE(f.values.at(1), QString());
        QVERIFY(!f.warning.isEmpty());
    }

    void nullFieldValuesAreEmptyAndNotAnError()
    {
        // An explicit null for one field means "not set", the same as absent.
        const auto f = profile("{\"displayname\":null,\"avatar_url\":\"mxc://h/a\"}");
        QCOMPARE(f.values.at(0), QString());
        QCOMPARE(f.values.at(1), QStringLiteral("mxc://h/a"));
        QVERIFY(f.warning.isEmpty());
    }

    // --- Everything else a server can put on the wire -------------------

    void malformedJsonWarnsAndStillAnswers()
    {
        for (const char* body : {"", "{", "not json", "{\"displayname\":}"}) {
            const auto f = profile(body);
            QCOMPARE(f.values.size(), 2);
            QCOMPARE(f.values.at(0), QString());
            QVERIFY2(!f.warning.isEmpty(), body);
        }
    }

    void nonObjectBodiesWarnAndStillAnswer()
    {
        // An array, a bare string and a number are all valid JSON, so they get
        // past parse() and have to be rejected on shape rather than by catch.
        for (const char* body : {"[]", R"(["Ada"])", R"("Ada")", "42", "true"}) {
            const auto f = profile(body);
            QCOMPARE(f.values.size(), 2);
            QCOMPARE(f.values.at(0), QString());
            QCOMPARE(f.values.at(1), QString());
            QVERIFY2(!f.warning.isEmpty(), body);
        }
    }

    void nonStringFieldWarnsButKeepsTheOtherField()
    {
        // One bad field must not cost the good one: a display name still
        // renders while the broken avatar falls back to an initial.
        const auto f = profile(R"({"displayname":"Ada","avatar_url":42})");
        QCOMPARE(f.values.at(0), QStringLiteral("Ada"));
        QCOMPARE(f.values.at(1), QString());
        QVERIFY(f.warning.contains(QStringLiteral("avatar_url")));
    }

    void warningNamesEveryBadField()
    {
        const auto f = profile(R"({"displayname":[],"avatar_url":{}})");
        QVERIFY(f.warning.contains(QStringLiteral("displayname")));
        QVERIFY(f.warning.contains(QStringLiteral("avatar_url")));
    }

    // --- The shape the nickname fetch uses ------------------------------

    void singleKeyRequestsGetASingleSlot()
    {
        auto f = readStringFields(R"({"nickname":"ada"})", {"nickname"});
        QCOMPARE(f.values.size(), 1);
        QCOMPARE(f.values.at(0), QStringLiteral("ada"));
        QVERIFY(f.warning.isEmpty());

        // ...and a null body still answers, so a nickname editor that waits on
        // the signal is never left disabled.
        f = readStringFields("null", {"nickname"});
        QCOMPARE(f.values.size(), 1);
        QCOMPARE(f.values.at(0), QString());
        QVERIFY(!f.warning.isEmpty());
    }
};

QTEST_MAIN(TestReplyFields)
#include "test_reply_fields.moc"

// Release-channel selection and semver precedence.
//
// Pure logic — no QNetworkAccessManager, no event loop, no GitHub. The
// updater's network half feeds a response body straight into
// parseReleaseList() + selectRelease(), so everything that decides what
// a user is offered is exercised here from a literal JSON string.
//
// The properties that matter, in the order they are easy to get wrong:
//   * 0.0.44-rc.1 sorts BELOW 0.0.44. A string compare says the
//     opposite and would present a downgrade as an update.
//   * rc.10 sorts ABOVE rc.9 (numeric identifiers compare numerically).
//   * `prerelease` — the boolean flag, never the tag text — decides who
//     may see a release.
//   * The list endpoint is scanned for a maximum, not indexed at [0].
//   * Opting out while on an RC offers nothing AND does not report
//     "up to date".
//   * A malformed release is skipped, not crashed on and not offered.

#include "core/ReleaseSelection.h"

#include <QTest>
#include <QVector>

using namespace bsfchat::updates;

namespace {

// Minimal GitHub-shaped release object. `prerelease` is written as a
// real JSON boolean because that is what the production parser demands.
QString rel(const QString& tag, bool prerelease,
            const QString& assetName = QStringLiteral("BSFChat-macOS.dmg"))
{
    QString assets;
    if (!assetName.isEmpty()) {
        assets = QStringLiteral(
            "\"assets\":[{\"name\":\"%1\","
            "\"browser_download_url\":\"https://example.invalid/%1\"}],")
            .arg(assetName);
    }
    return QStringLiteral(
        "{\"tag_name\":\"%1\",\"prerelease\":%2,\"draft\":false,"
        "%3\"html_url\":\"https://example.invalid/r/%1\","
        "\"body\":\"notes for %1\"}")
        .arg(tag, prerelease ? QStringLiteral("true") : QStringLiteral("false"),
             assets);
}

QByteArray list(const QStringList& entries)
{
    return (QLatin1Char('[') + entries.join(QLatin1Char(','))
            + QLatin1Char(']')).toUtf8();
}

} // namespace

class TestUpdateChannel : public QObject {
    Q_OBJECT

private slots:
    // ---- Version parsing -------------------------------------------

    void parsesPlainAndPrefixedVersions() {
        const Version a = parseVersion(QStringLiteral("0.0.44"));
        QVERIFY(a.valid);
        QCOMPARE(a.major, 0); QCOMPARE(a.minor, 0); QCOMPARE(a.patch, 44);
        QVERIFY(!a.isPrerelease());

        const Version b = parseVersion(QStringLiteral("v1.2.3"));
        QVERIFY(b.valid);
        QCOMPARE(b.major, 1); QCOMPARE(b.minor, 2); QCOMPARE(b.patch, 3);
    }

    void parsesPrereleaseIdentifiers() {
        const Version v = parseVersion(QStringLiteral("0.0.44-rc.2"));
        QVERIFY(v.valid);
        QVERIFY(v.isPrerelease());
        QCOMPARE(v.pre.size(), 2);
        QCOMPARE(v.pre.at(0), QStringLiteral("rc"));
        QCOMPARE(v.pre.at(1), QStringLiteral("2"));
    }

    // Build metadata takes no part in precedence (semver §10).
    //
    // The validity assertion is load-bearing and was added after a
    // mutation survived: with the `+` never stripped, "0.0.44+abc"
    // fails to parse, and two UNPARSEABLE versions also compare equal —
    // so an equality-only test passes while the feature is broken.
    void buildMetadataIsIgnored() {
        QVERIFY(parseVersion(QStringLiteral("0.0.44+abc")).valid);
        QCOMPARE(parseVersion(QStringLiteral("0.0.44+abc")).patch, 44);
        QVERIFY(!parseVersion(QStringLiteral("0.0.44+abc")).isPrerelease());

        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44+abc"),
                                       QStringLiteral("0.0.44+zzz")), 0);
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44+abc"),
                                       QStringLiteral("0.0.44")), 0);
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.45+abc"),
                                       QStringLiteral("0.0.44")), 1);
        // Metadata after a prerelease suffix is still only metadata.
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-rc.1+abc"),
                                       QStringLiteral("0.0.44")), -1);
    }

    void rejectsMalformedVersions_data() {
        QTest::addColumn<QString>("text");
        QTest::newRow("empty")            << QString();
        QTest::newRow("words")            << QStringLiteral("nightly");
        QTest::newRow("two components")   << QStringLiteral("1.2");
        QTest::newRow("four components")  << QStringLiteral("1.2.3.4");
        QTest::newRow("non-numeric core") << QStringLiteral("1.x.3");
        QTest::newRow("trailing dash")    << QStringLiteral("1.2.3-");
        QTest::newRow("empty pre id")     << QStringLiteral("1.2.3-rc..1");
        QTest::newRow("bad pre char")     << QStringLiteral("1.2.3-rc_1");
        QTest::newRow("overflow")         << QStringLiteral("99999999999.0.0");
        QTest::newRow("v only")           << QStringLiteral("v");
    }
    void rejectsMalformedVersions() {
        QFETCH(QString, text);
        QVERIFY2(!parseVersion(text).valid, qPrintable(text));
    }

    // ---- Precedence ------------------------------------------------

    // The headline ordering. A naive string compare gets the first row
    // backwards, which is what makes it dangerous rather than merely
    // wrong: it turns 0.0.44-rc.1 into an "update" for someone already
    // on 0.0.44.
    void prereleaseSortsBelowItsRelease() {
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-rc.1"),
                                       QStringLiteral("0.0.44")), -1);
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44"),
                                       QStringLiteral("0.0.44-rc.1")), 1);
        // ...and the chain the requirement names, end to end.
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44"),
                                       QStringLiteral("0.0.45-rc.1")), -1);
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-rc.1"),
                                       QStringLiteral("0.0.45-rc.1")), -1);
    }

    void laterRcSortsAboveEarlierRc() {
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-rc.2"),
                                       QStringLiteral("0.0.44-rc.1")), 1);
    }

    // Numeric identifiers compare numerically, so rc.10 > rc.9. Lexical
    // ordering says "10" < "9" and would strand testers on rc.9.
    void numericIdentifiersCompareNumerically() {
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-rc.10"),
                                       QStringLiteral("0.0.44-rc.9")), 1);
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-rc.9"),
                                       QStringLiteral("0.0.44-rc.10")), -1);
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-rc.100"),
                                       QStringLiteral("0.0.44-rc.99")), 1);
    }

    // Mixed identifier kinds: numeric always ranks below alphanumeric
    // (semver §11.4.3), and a longer identifier list wins a tie.
    void numericRanksBelowAlphanumeric() {
        QCOMPARE(compareVersionStrings(QStringLiteral("1.0.0-1"),
                                       QStringLiteral("1.0.0-alpha")), -1);
        QCOMPARE(compareVersionStrings(QStringLiteral("1.0.0-alpha"),
                                       QStringLiteral("1.0.0-beta")), -1);
        QCOMPARE(compareVersionStrings(QStringLiteral("1.0.0-rc.1.1"),
                                       QStringLiteral("1.0.0-rc.1")), 1);
        // "dev" < "rc": an unreleased CI build never outranks the RC it
        // was cut from, which is what keeps -dev builds updatable.
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44-dev.abc123"),
                                       QStringLiteral("0.0.44-rc.1")), -1);
    }

    void coreComponentsOutrankEverything() {
        QCOMPARE(compareVersionStrings(QStringLiteral("0.1.0"),
                                       QStringLiteral("0.0.99")), 1);
        QCOMPARE(compareVersionStrings(QStringLiteral("1.0.0-rc.1"),
                                       QStringLiteral("0.9.9")), 1);
        QCOMPARE(compareVersionStrings(QStringLiteral("0.0.44"),
                                       QStringLiteral("0.0.44")), 0);
    }

    // ---- Channel plumbing ------------------------------------------

    void channelDefaultsToStable() {
        QVERIFY(channelFromString(QString()) == Channel::Stable);
        QVERIFY(channelFromString(QStringLiteral("stable")) == Channel::Stable);
        QVERIFY(channelFromString(QStringLiteral("nonsense")) == Channel::Stable);
        QVERIFY(channelFromString(QStringLiteral("beta")) == Channel::Beta);
        QVERIFY(channelFromString(QStringLiteral(" BETA ")) == Channel::Beta);
        QCOMPARE(channelToString(Channel::Beta), QStringLiteral("beta"));
        QCOMPARE(channelToString(Channel::Stable), QStringLiteral("stable"));
    }

    // ---- Selection: the RC is offered on beta, invisible on stable --

    void rcIsOfferedOnBeta() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.1"), true),
            rel(QStringLiteral("v0.0.44"), false),
        }));
        QCOMPARE(releases.size(), 2);

        const Selection s = selectRelease(releases, Channel::Beta,
                                          QStringLiteral("0.0.44"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.45-rc.1"));
        QVERIFY(s.release.prerelease);
    }

    void rcIsInvisibleOnStable() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.1"), true),
            rel(QStringLiteral("v0.0.44"), false),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.44"));
        QVERIFY(s.outcome == Outcome::UpToDate);
        // Not merely "not offered" — the RC must not surface anywhere a
        // stable user's UI could read it.
        QCOMPARE(s.newestChannelTag, QStringLiteral("v0.0.44"));
        QVERIFY(s.release.tag.isEmpty());
    }

    // A stable release published after an RC supersedes it on beta too.
    void betaTakesTheStableReleaseWhenItIsNewer() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.44"), false),
            rel(QStringLiteral("v0.0.44-rc.2"), true),
        }));
        const Selection s = selectRelease(releases, Channel::Beta,
                                          QStringLiteral("0.0.44-rc.2"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.44"));
    }

    // ---- The `prerelease` flag is the authority --------------------

    // A tag that merely looks like an RC but is flagged as a full
    // release is a full release. Substring-matching "rc" in the tag
    // would hide it from everyone on stable.
    void tagTextDoesNotOverrideTheFlag() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.1"), false),   // flagged STABLE
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.44"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.45-rc.1"));
    }

    // ...and the converse: an ordinary-looking tag flagged as a
    // prerelease stays hidden from stable users.
    void anOrdinaryTagFlaggedPrereleaseStaysHidden() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45"), true),         // flagged PRERELEASE
            rel(QStringLiteral("v0.0.44"), false),
        }));
        QVERIFY(selectRelease(releases, Channel::Stable,
                              QStringLiteral("0.0.44")).outcome
                == Outcome::UpToDate);
        const Selection beta = selectRelease(releases, Channel::Beta,
                                             QStringLiteral("0.0.44"));
        QVERIFY(beta.outcome == Outcome::UpdateAvailable);
        QCOMPARE(beta.release.tag, QStringLiteral("v0.0.45"));
    }

    // A release whose `prerelease` field is absent or not a boolean
    // cannot be classified, so it is dropped rather than defaulting to
    // "stable" — defaulting is how an RC leaks to everyone.
    void unclassifiableReleasesAreDropped() {
        const QByteArray body =
            "[{\"tag_name\":\"v0.0.45\",\"draft\":false},"
            " {\"tag_name\":\"v0.0.46\",\"prerelease\":\"true\",\"draft\":false},"
            " {\"tag_name\":\"v0.0.44\",\"prerelease\":false,\"draft\":false}]";
        const auto releases = parseReleaseList(body);
        QCOMPARE(releases.size(), 1);
        QCOMPARE(releases.at(0).tag, QStringLiteral("v0.0.44"));
    }

    void draftsAreDropped() {
        const QByteArray body =
            "[{\"tag_name\":\"v0.0.99\",\"prerelease\":false,\"draft\":true},"
            " {\"tag_name\":\"v0.0.44\",\"prerelease\":false,\"draft\":false}]";
        const auto releases = parseReleaseList(body);
        QCOMPARE(releases.size(), 1);
        QCOMPARE(releases.at(0).tag, QStringLiteral("v0.0.44"));
    }

    // ---- Ordering / pagination -------------------------------------

    // GitHub orders the list by creation date, which is not version
    // order: a backported patch, a re-cut release or a late-promoted
    // draft all put a lower version at index 0. Taking [0] would offer
    // the wrong build — or, when [0] is older than the running build,
    // report "up to date" while a newer release sits further down.
    void newestIsFoundRegardlessOfListOrder() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.43")   , false),   // newest by DATE
            rel(QStringLiteral("v0.0.45")   , false),   // newest by VERSION
            rel(QStringLiteral("v0.0.44")   , false),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.43"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.45"));
    }

    void newestRcIsFoundRegardlessOfListOrder() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.9"),  true),
            rel(QStringLiteral("v0.0.45-rc.10"), true),
            rel(QStringLiteral("v0.0.45-rc.2"),  true),
        }));
        const Selection s = selectRelease(releases, Channel::Beta,
                                          QStringLiteral("0.0.45-rc.9"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.45-rc.10"));
    }

    // ---- Opt-out while running an RC -------------------------------

    // The product decision: stay put. No downgrade is offered...
    void optingOutOnAnRcOffersNothing() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.1"), true),
            rel(QStringLiteral("v0.0.44"), false),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.45-rc.1"));
        QVERIFY(s.release.tag.isEmpty());
        QVERIFY(s.outcome != Outcome::UpdateAvailable);
    }

    // ...and the state is NOT UpToDate, because the user is running
    // something ahead of the channel and is owed that distinction
    // rather than a claim that turns out to be false the moment they
    // compare notes with anyone on stable.
    void optingOutOnAnRcReportsAheadOfChannel() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.1"), true),
            rel(QStringLiteral("v0.0.44"), false),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.45-rc.1"));
        QVERIFY(s.outcome == Outcome::AheadOfChannel);
        // The UI names what it is ahead of.
        QCOMPARE(s.newestStableTag, QStringLiteral("v0.0.44"));
    }

    // The moment stable passes the RC, the normal flow resumes — this
    // is the "no updates until a stable release exceeds it" half.
    void aStableReleaseThatExceedsTheRcIsOffered() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45"), false),
            rel(QStringLiteral("v0.0.45-rc.1"), true),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.45-rc.1"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.45"));
    }

    // Same shape, one patch lower: 0.0.44 stable vs 0.0.44-rc.1 running.
    // This is the case a string compare inverts.
    void theFinalReleaseSupersedesItsOwnRc() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.44"), false),
            rel(QStringLiteral("v0.0.44-rc.1"), true),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.44-rc.1"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.44"));
    }

    // Being ahead is about the running build, not the channel setting:
    // a beta user on a locally-built 0.1.0-rc.1 gets the same honest
    // state rather than a silent "up to date".
    void aheadOfChannelAlsoAppliesOnBeta() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.1"), true),
            rel(QStringLiteral("v0.0.44"), false),
        }));
        const Selection s = selectRelease(releases, Channel::Beta,
                                          QStringLiteral("0.1.0-rc.1"));
        QVERIFY(s.outcome == Outcome::AheadOfChannel);
    }

    // A STABLE build ahead of the channel is just "up to date" — there
    // is no prerelease story to explain, and warning about it would be
    // noise on every dev machine between tag and publish.
    void aStableBuildAheadOfTheChannelIsUpToDate() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.44"), false),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.45"));
        QVERIFY(s.outcome == Outcome::UpToDate);
    }

    // ---- Malformed input -------------------------------------------

    void malformedTagsAreSkippedNotOffered() {
        const QByteArray body =
            "[{\"tag_name\":\"nightly\",\"prerelease\":false,\"draft\":false},"
            " {\"tag_name\":\"\",\"prerelease\":false,\"draft\":false},"
            " {\"prerelease\":false,\"draft\":false},"
            " {\"tag_name\":42,\"prerelease\":false,\"draft\":false},"
            " {\"tag_name\":\"v0.0.44\",\"prerelease\":false,\"draft\":false}]";
        const auto releases = parseReleaseList(body);
        QCOMPARE(releases.size(), 1);
        QCOMPARE(releases.at(0).tag, QStringLiteral("v0.0.44"));

        // And the one good entry still drives a correct decision.
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.43"));
        QVERIFY(s.outcome == Outcome::UpdateAvailable);
        QCOMPARE(s.release.tag, QStringLiteral("v0.0.44"));
    }

    // Nothing usable at all is NOT "up to date": we learned nothing, so
    // claiming currency would be an invention.
    void anAllMalformedListYieldsNoVerdict() {
        const QByteArray body =
            "[{\"tag_name\":\"nightly\",\"prerelease\":false,\"draft\":false}]";
        const auto releases = parseReleaseList(body);
        QVERIFY(releases.isEmpty());
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("0.0.44"));
        QVERIFY(s.outcome == Outcome::NoUsableRelease);
        QVERIFY(s.release.tag.isEmpty());
    }

    void garbageBodiesAreReportedNotCrashed() {
        QString err;
        QVERIFY(parseReleaseList(QByteArray("not json at all"), &err).isEmpty());
        QVERIFY(!err.isEmpty());

        err.clear();
        // The old /releases/latest shape: a bare object, not a list.
        QVERIFY(parseReleaseList(QByteArray("{\"tag_name\":\"v0.0.44\"}"), &err)
                    .isEmpty());
        QVERIFY(!err.isEmpty());

        err.clear();
        QVERIFY(parseReleaseList(QByteArray("[]"), &err).isEmpty());
        QVERIFY(err.isEmpty());   // empty list is valid, just uninformative

        err.clear();
        QVERIFY(parseReleaseList(QByteArray("[1,2,\"three\",null]"), &err)
                    .isEmpty());
        QVERIFY(err.isEmpty());
    }

    // If our OWN version doesn't parse we cannot compare, so we decline
    // to decide rather than offering an arbitrary release as an
    // "update" off an unknown baseline.
    void anUnparseableRunningVersionOffersNothing() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.44"), false),
        }));
        const Selection s = selectRelease(releases, Channel::Stable,
                                          QStringLiteral("not-a-version"));
        QVERIFY(s.outcome == Outcome::NoUsableRelease);
        QVERIFY(s.release.tag.isEmpty());
    }

    // ---- Assets ----------------------------------------------------

    void assetLookupIsByExactName() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45"), false,
                QStringLiteral("BSFChat-macOS.dmg")),
        }));
        QCOMPARE(releases.size(), 1);
        QCOMPARE(assetUrlNamed(releases.at(0),
                               QStringLiteral("BSFChat-macOS.dmg")),
                 QStringLiteral("https://example.invalid/BSFChat-macOS.dmg"));
        QVERIFY(assetUrlNamed(releases.at(0),
                              QStringLiteral("BSFChat-Setup.exe")).isEmpty());
        // An empty platform suffix (unsupported OS) must not match the
        // first asset by accident.
        QVERIFY(assetUrlNamed(releases.at(0), QString()).isEmpty());
    }

    void releaseMetadataSurvivesParsing() {
        const auto releases = parseReleaseList(list({
            rel(QStringLiteral("v0.0.45-rc.1"), true),
        }));
        QCOMPARE(releases.size(), 1);
        QCOMPARE(releases.at(0).htmlUrl,
                 QStringLiteral("https://example.invalid/r/v0.0.45-rc.1"));
        QCOMPARE(releases.at(0).notes,
                 QStringLiteral("notes for v0.0.45-rc.1"));
    }

    // ---- Build badge -----------------------------------------------

    void buildLabelIdentifiesPrereleaseBuilds() {
        QCOMPARE(buildChannelLabel(QStringLiteral("0.0.45-rc.1")),
                 QStringLiteral("RC"));
        QCOMPARE(buildChannelLabel(QStringLiteral("0.0.45-beta.3")),
                 QStringLiteral("BETA"));
        QCOMPARE(buildChannelLabel(QStringLiteral("0.0.45-dev.abc123")),
                 QStringLiteral("DEV"));
        QCOMPARE(buildChannelLabel(QStringLiteral("0.0.45-nightly")),
                 QStringLiteral("PRE"));
        // A plain release gets no badge at all — the badge has to mean
        // something, so it must be absent on ordinary builds.
        QVERIFY(buildChannelLabel(QStringLiteral("0.0.45")).isEmpty());
        QVERIFY(buildChannelLabel(QStringLiteral("garbage")).isEmpty());
    }
};

QTEST_MAIN(TestUpdateChannel)
#include "test_update_channel.moc"

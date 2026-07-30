// Release-channel selection and semver precedence — the decision half
// of the auto-updater, deliberately kept free of QNetworkAccessManager
// so it can be unit-tested without a network, an event loop, or a
// GitHub token. Updater.cpp does the I/O and calls in here to decide
// what (if anything) to offer.
//
// Header-only and `inline` on purpose: adding a .cpp would mean adding
// a source line to the app target's CMakeLists, and this file exists to
// be included by both `bsfchat-app` and `test_update_channel` with no
// build-graph coupling between them.
//
// Why the /releases list instead of /releases/latest:
//   GitHub's /releases/latest endpoint *excludes prereleases entirely* —
//   there is no query parameter that makes it return one. Beta support
//   therefore is not a flag on the old call; it is a different endpoint
//   (/releases, the list) plus a filter on each entry's `prerelease`
//   boolean. Still exactly one GET per check, so the 60 req/hour
//   unauthenticated budget and the 6-hour throttle are unaffected; the
//   response body is bigger (up to `per_page` releases instead of one).
//
// The list endpoint returns newest-*created* first, which is not the
// same as highest-version first (a backported 0.0.43.1 patch cut after
// 0.0.44, a re-published release, a draft promoted late). So we never
// take element [0] — we scan every entry and take the maximum under
// semver precedence. Pagination is deliberately not followed: one page
// is one request, and a release that fell off page one is older than
// `per_page` more recent releases and cannot be the newest.
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace bsfchat::updates {

// ---------------------------------------------------------------------
// Semantic version with prerelease precedence (semver.org §11).
//
// This replaces a comparison that parsed `major.minor.patch` and threw
// the `-suffix` away. That was fine while every published tag was a
// plain release: it made 0.0.44-rc.1 compare *equal* to 0.0.44. The
// moment RCs are published it is actively wrong — a user on 0.0.44
// would be told 0.0.44-rc.1 is "not newer" (correct by accident) while
// a user on 0.0.44-rc.1 would be told 0.0.44 is "not newer" either,
// stranding every tester one release behind forever.
// ---------------------------------------------------------------------
struct Version {
    bool valid = false;
    int major = 0;
    int minor = 0;
    int patch = 0;
    // Dot-separated prerelease identifiers, e.g. {"rc","2"}. Empty for a
    // stable version. Build metadata (`+sha`) is parsed off and dropped:
    // semver says it takes no part in precedence.
    QStringList pre;

    bool isPrerelease() const { return !pre.isEmpty(); }
};

// Parse "v0.0.44-rc.2+build" / "0.0.44" / "0.0.44-dev.abc123".
// Anything that is not exactly three numeric components (with an
// optional well-formed prerelease) comes back with valid == false. We
// are strict on purpose: an unparseable tag must be *skipped*, never
// coerced into a number that could be offered as an update.
inline Version parseVersion(const QString& in)
{
    Version v;
    QString s = in.trimmed();
    if (s.isEmpty()) return v;
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s = s.mid(1);

    // Build metadata: everything from the first '+' is ignored.
    const qsizetype plus = s.indexOf(QLatin1Char('+'));
    if (plus >= 0) s = s.left(plus);

    QString pre;
    const qsizetype dash = s.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        pre = s.mid(dash + 1);
        s = s.left(dash);
    }

    const QStringList core = s.split(QLatin1Char('.'));
    if (core.size() != 3) return v;
    int out[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        // value(), not at(): if the size guard above is ever loosened,
        // a short list must read back as an empty component and be
        // rejected below, not as an out-of-bounds read.
        const QString part = core.value(i);
        if (part.isEmpty()) return v;
        for (const QChar c : part)
            if (!c.isDigit()) return v;
        bool ok = false;
        // Overflow (a 12-digit "version") fails here rather than wrapping.
        const int n = part.toInt(&ok);
        if (!ok) return v;
        out[i] = n;
    }

    if (dash >= 0) {
        if (pre.isEmpty()) return v;              // trailing '-'
        const QStringList ids = pre.split(QLatin1Char('.'));
        for (const QString& id : ids) {
            if (id.isEmpty()) return v;           // ".." or trailing '.'
            for (const QChar c : id) {
                const bool okChar = c.isDigit()
                    || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                    || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                    || c == QLatin1Char('-');
                if (!okChar) return v;
            }
        }
        v.pre = ids;
    }

    v.major = out[0];
    v.minor = out[1];
    v.patch = out[2];
    v.valid = true;
    return v;
}

namespace detail {

inline bool isNumericId(const QString& s)
{
    if (s.isEmpty()) return false;
    for (const QChar c : s)
        if (!c.isDigit()) return false;
    return true;
}

} // namespace detail

// -1 / 0 / +1. Both arguments must be valid; comparing an invalid
// Version is a caller bug, so we treat invalid as lowest rather than
// asserting (a malformed tag must never win a max()).
inline int compareVersions(const Version& a, const Version& b)
{
    if (!a.valid || !b.valid) {
        if (a.valid == b.valid) return 0;
        return a.valid ? 1 : -1;
    }
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;

    // Same core. A prerelease has LOWER precedence than the release it
    // precedes: 0.0.44-rc.1 < 0.0.44. This is the case a string compare
    // gets backwards ("0.0.44-rc.1" > "0.0.44" lexically), which would
    // present a downgrade as an update.
    const bool ap = a.isPrerelease(), bp = b.isPrerelease();
    if (!ap && !bp) return 0;
    if (!ap) return 1;
    if (!bp) return -1;

    const qsizetype n = std::min(a.pre.size(), b.pre.size());
    for (qsizetype i = 0; i < n; ++i) {
        const QString& x = a.pre.at(i);
        const QString& y = b.pre.at(i);
        const bool xn = detail::isNumericId(x), yn = detail::isNumericId(y);
        if (xn && yn) {
            // Numerically, so rc.10 > rc.9 — the comparison a lexical
            // sort gets wrong ("10" < "9").
            const qlonglong xv = x.toLongLong();
            const qlonglong yv = y.toLongLong();
            if (xv != yv) return xv < yv ? -1 : 1;
        } else if (xn != yn) {
            // Numeric identifiers always rank below alphanumeric ones.
            return xn ? -1 : 1;
        } else {
            const int c = QString::compare(x, y);   // ASCII order
            if (c != 0) return c < 0 ? -1 : 1;
        }
    }
    // All shared fields equal: more fields wins (rc.1.1 > rc.1).
    if (a.pre.size() != b.pre.size())
        return a.pre.size() < b.pre.size() ? -1 : 1;
    return 0;
}

inline int compareVersionStrings(const QString& a, const QString& b)
{
    return compareVersions(parseVersion(a), parseVersion(b));
}

// ---------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------
enum class Channel {
    Stable,   // published, non-prerelease tags only
    Beta,     // stable tags AND prereleases (RCs), newest of either wins
};

inline QString channelToString(Channel c)
{
    return c == Channel::Beta ? QStringLiteral("beta") : QStringLiteral("stable");
}

// Anything we don't recognise falls back to stable. A corrupt or
// hand-edited settings file must not silently opt someone into betas.
inline Channel channelFromString(const QString& s)
{
    return s.trimmed().compare(QLatin1String("beta"), Qt::CaseInsensitive) == 0
        ? Channel::Beta : Channel::Stable;
}

// ---------------------------------------------------------------------
// Releases
// ---------------------------------------------------------------------
struct ReleaseAsset {
    QString name;
    QString url;    // browser_download_url
};

struct Release {
    QString tag;
    Version version;
    bool prerelease = false;
    QString htmlUrl;
    QString notes;
    QVector<ReleaseAsset> assets;
};

// Parse the body of GET /repos/:owner/:repo/releases.
//
// Every entry is validated independently and a bad one is skipped, not
// fatal: one malformed release in the list must not blind the updater to
// the twenty-nine good ones. An entry is skipped when it
//   * is not a JSON object,
//   * has a missing / empty / unparseable `tag_name`,
//   * is a draft, or
//   * has no boolean `prerelease` field.
// That last rule is the point of the whole feature: the flag is the
// authority for what is and isn't a beta, so an entry that cannot state
// it is not classifiable and gets dropped rather than defaulting to
// "stable" and leaking an RC to everyone.
inline QVector<Release> parseReleaseList(const QByteArray& body, QString* error = nullptr)
{
    QVector<Release> out;
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError) {
        if (error) *error = perr.errorString();
        return out;
    }
    if (!doc.isArray()) {
        if (error) *error = QStringLiteral("expected a JSON array of releases");
        return out;
    }

    const QJsonArray arr = doc.array();
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();

        if (o.value(QStringLiteral("draft")).toBool(false)) continue;

        const QJsonValue preFlag = o.value(QStringLiteral("prerelease"));
        if (!preFlag.isBool()) continue;

        const QJsonValue tagVal = o.value(QStringLiteral("tag_name"));
        if (!tagVal.isString()) continue;
        const QString tag = tagVal.toString();
        const Version ver = parseVersion(tag);
        if (!ver.valid) continue;

        Release r;
        r.tag = tag;
        r.version = ver;
        r.prerelease = preFlag.toBool();
        r.htmlUrl = o.value(QStringLiteral("html_url")).toString();
        r.notes = o.value(QStringLiteral("body")).toString();
        for (const QJsonValue& av : o.value(QStringLiteral("assets")).toArray()) {
            if (!av.isObject()) continue;
            const QJsonObject a = av.toObject();
            ReleaseAsset asset;
            asset.name = a.value(QStringLiteral("name")).toString();
            asset.url = a.value(QStringLiteral("browser_download_url")).toString();
            if (!asset.name.isEmpty()) r.assets.append(asset);
        }
        out.append(r);
    }
    if (error) error->clear();
    return out;
}

enum class Outcome {
    // Nothing usable came back — empty list, all entries malformed, or
    // our own version string doesn't parse. Distinct from UpToDate: we
    // learned nothing, so we must not claim to be current.
    NoUsableRelease,
    UpToDate,
    UpdateAvailable,
    // Running a build that is newer than the newest release on the
    // selected channel. In practice: a tester who opted out of beta
    // while on an RC. Per product decision there is no downgrade — they
    // keep the build and wait for stable to catch up — but this is NOT
    // "up to date" and must not be worded as such.
    AheadOfChannel,
};

struct Selection {
    Outcome outcome = Outcome::NoUsableRelease;
    Release release;              // populated only when UpdateAvailable
    QString newestChannelTag;     // newest tag on the selected channel, if any
    QString newestStableTag;      // newest non-prerelease tag, if any
};

// Decide what to do given every release we can see, the user's channel,
// and the running build's version string.
//
// Stable users never see a prerelease here at all — not "see it and
// reject it", it is filtered before comparison — so no RC tag, note or
// asset URL can reach a stable user's UI.
inline Selection selectRelease(const QVector<Release>& releases,
                               Channel channel,
                               const QString& currentVersion)
{
    Selection sel;
    const Version cur = parseVersion(currentVersion);

    const Release* bestChannel = nullptr;
    const Release* bestStable = nullptr;
    for (const Release& r : releases) {
        if (!r.version.valid) continue;
        if (!r.prerelease) {
            if (!bestStable || compareVersions(r.version, bestStable->version) > 0)
                bestStable = &r;
        }
        const bool eligible = (channel == Channel::Beta) || !r.prerelease;
        if (!eligible) continue;
        if (!bestChannel || compareVersions(r.version, bestChannel->version) > 0)
            bestChannel = &r;
    }

    if (bestChannel) sel.newestChannelTag = bestChannel->tag;
    if (bestStable) sel.newestStableTag = bestStable->tag;

    // Our own version being unparseable is a build-configuration fault.
    // Offering "an update" off an unknown baseline could hand someone a
    // downgrade, so we decline to decide.
    if (!cur.valid || !bestChannel) {
        sel.outcome = Outcome::NoUsableRelease;
        return sel;
    }

    const int cmp = compareVersions(bestChannel->version, cur);
    if (cmp > 0) {
        sel.outcome = Outcome::UpdateAvailable;
        sel.release = *bestChannel;
        return sel;
    }
    if (cmp < 0 && cur.isPrerelease()) {
        // Newer than everything our channel offers, and we're on a
        // prerelease build — the opt-out-while-on-an-RC case.
        sel.outcome = Outcome::AheadOfChannel;
        return sel;
    }
    sel.outcome = Outcome::UpToDate;
    return sel;
}

// Pick the asset for this platform out of a chosen release. Exact-name
// match, same as the previous /releases/latest code path.
inline QString assetUrlNamed(const Release& r, const QString& assetName)
{
    if (assetName.isEmpty()) return QString();
    for (const ReleaseAsset& a : r.assets)
        if (a.name == assetName) return a.url;
    return QString();
}

// "RC" / "DEV" / "PRE" / "" for the running build, so the UI can badge a
// prerelease at a glance instead of relying on the reader to notice a
// `-rc.2` suffix on the version string.
inline QString buildChannelLabel(const QString& version)
{
    const Version v = parseVersion(version);
    if (!v.valid || !v.isPrerelease()) return QString();
    const QString first = v.pre.first().toLower();
    if (first.startsWith(QLatin1String("rc"))) return QStringLiteral("RC");
    if (first.startsWith(QLatin1String("dev"))) return QStringLiteral("DEV");
    if (first.startsWith(QLatin1String("beta"))) return QStringLiteral("BETA");
    if (first.startsWith(QLatin1String("alpha"))) return QStringLiteral("ALPHA");
    return QStringLiteral("PRE");
}

} // namespace bsfchat::updates

#include "core/AppProfile.h"

namespace bsfchat {

namespace {

constexpr int kMaxProfileLength = 32;
const QString kBaseName = QStringLiteral("BSFChat");

QString g_activeProfile;

bool isSafeChar(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
        || c == QLatin1Char('.') || c == QLatin1Char('_')
        || c == QLatin1Char('-');
}

bool isSeparator(QChar c)
{
    return c == QLatin1Char('.') || c == QLatin1Char('_')
        || c == QLatin1Char('-');
}

} // namespace

QString sanitizeProfileName(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    for (QChar c : raw)
        out.append(isSafeChar(c) ? c : QLatin1Char('_'));

    while (!out.isEmpty() && isSeparator(out.front()))
        out.remove(0, 1);
    while (!out.isEmpty() && isSeparator(out.back()))
        out.chop(1);

    if (out.size() > kMaxProfileLength)
        out.truncate(kMaxProfileLength);
    while (!out.isEmpty() && isSeparator(out.back()))
        out.chop(1);

    // A name that sanitized down to nothing but underscores carries no
    // information and would silently collide with every other such name;
    // treat it as "no profile" rather than inventing one.
    bool anyAlnum = false;
    for (QChar c : out) {
        if (!isSeparator(c)) { anyAlnum = true; break; }
    }
    return anyAlnum ? out : QString();
}

QString profileFromArguments(int argc, char** argv, const QByteArray& envValue)
{
    static const QString kFlag = QStringLiteral("--profile");
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == kFlag) {
            if (i + 1 < argc)
                return sanitizeProfileName(QString::fromLocal8Bit(argv[i + 1]));
            return {}; // trailing --profile with no value
        }
        if (a.startsWith(kFlag + QLatin1Char('=')))
            return sanitizeProfileName(a.mid(kFlag.size() + 1));
    }
    return sanitizeProfileName(QString::fromLocal8Bit(envValue));
}

QString organizationNameForProfile(const QString& /*profile*/)
{
    // The organization stays "BSFChat" for every profile: on macOS the
    // org name also feeds the bundle/defaults domain, and keeping it
    // stable means a profile only ever adds a leaf directory.
    return kBaseName;
}

QString applicationNameForProfile(const QString& profile)
{
    const QString p = sanitizeProfileName(profile);
    return p.isEmpty() ? kBaseName : kBaseName + QLatin1Char('-') + p;
}

QString socketSuffixForProfile(const QString& profile)
{
    const QString p = sanitizeProfileName(profile);
    return p.isEmpty() ? QString() : QLatin1Char('-') + p;
}

void setActiveProfile(const QString& profile)
{
    g_activeProfile = sanitizeProfileName(profile);
}

QString activeProfile()
{
    return g_activeProfile;
}

QString organizationName()
{
    return organizationNameForProfile(g_activeProfile);
}

QString applicationName()
{
    return applicationNameForProfile(g_activeProfile);
}

} // namespace bsfchat

#include "UpdateIntegrity.h"

#include "ReleaseSigningKey.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <openssl/err.h>
#include <openssl/evp.h>

namespace bsfchat::updates {

namespace {

bool baseUrlChecks(const QUrl& url)
{
    return url.isValid()
        && url.scheme() == QLatin1String("https")
        && (url.port() == -1 || url.port() == 443)
        && url.userInfo().isEmpty();
}

bool isHexDigest(const QByteArray& s)
{
    if (s.size() != 64) return false;
    for (const char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                     || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

void setErr(QString* error, const QString& msg)
{
    if (error) *error = msg;
}

} // namespace

bool isAllowedAssetUrl(const QUrl& url)
{
    if (!baseUrlChecks(url)) return false;
    if (url.host().compare(QLatin1String("github.com"), Qt::CaseInsensitive) != 0)
        return false;
    // /BSFChat/client/releases/download/<tag>/<name>. FullyDecoded so an
    // encoded "%2F.." cannot step out of the tree after the check.
    const QString path = url.path(QUrl::FullyDecoded);
    static const QLatin1String prefix("/BSFChat/client/releases/download/");
    if (!path.startsWith(prefix)) return false;
    const QStringList rest = path.mid(prefix.size()).split(QLatin1Char('/'));
    if (rest.size() != 2) return false;
    for (const QString& part : rest)
        if (part.isEmpty() || part == QLatin1String(".")
            || part == QLatin1String("..") || part.contains(QLatin1Char('\\')))
            return false;
    return true;
}

bool isAllowedRedirectUrl(const QUrl& url)
{
    if (!baseUrlChecks(url)) return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("github.com")
        || host == QLatin1String("release-assets.githubusercontent.com")
        || host == QLatin1String("objects.githubusercontent.com");
}

bool verifyEd25519(const QByteArray& message, const QByteArray& signature,
                   const QList<QByteArray>& publicKeysRaw, QString* error)
{
    if (signature.size() != kEd25519SignatureBytes) {
        setErr(error, QStringLiteral("signature is %1 bytes, expected %2")
                          .arg(signature.size()).arg(kEd25519SignatureBytes));
        return false;
    }
    bool ok = false;
    for (const QByteArray& key : publicKeysRaw) {
        if (key.size() != kEd25519PublicKeyBytes) continue;
        EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char*>(key.constData()),
            static_cast<size_t>(key.size()));
        if (!pkey) continue;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (ctx
            && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1
            && EVP_DigestVerify(
                   ctx,
                   reinterpret_cast<const unsigned char*>(signature.constData()),
                   static_cast<size_t>(signature.size()),
                   reinterpret_cast<const unsigned char*>(message.constData()),
                   static_cast<size_t>(message.size())) == 1) {
            ok = true;
        }
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        if (ok) break;
    }
    // A failed verify leaves entries on the thread's OpenSSL error queue;
    // don't let them surface later as a bogus TLS error elsewhere.
    ERR_clear_error();
    if (!ok)
        setErr(error, QStringLiteral("signature does not match any release key"));
    return ok;
}

QList<QByteArray> releaseSigningKeys()
{
    QList<QByteArray> keys;
    for (const char* hex : kReleaseSigningKeysHex)
        keys.append(QByteArray::fromHex(QByteArray(hex)));
    return keys;
}

std::optional<Manifest> parseManifest(const QByteArray& body, QString* error)
{
    if (body.size() > kMaxManifestBytes) {
        setErr(error, QStringLiteral("manifest too large"));
        return std::nullopt;
    }
    Manifest m;
    bool haveTag = false;
    int lineNo = 0;
    for (QByteArray line : body.split('\n')) {
        ++lineNo;
        if (line.endsWith('\r')) line.chop(1);
        if (line.trimmed().isEmpty()) continue;
        if (line.startsWith('#')) {
            static const QByteArray tagKey("# release:");
            if (line.startsWith(tagKey)) {
                const QString tag = QString::fromUtf8(line.mid(tagKey.size())).trimmed();
                if (haveTag || tag.isEmpty()) {
                    setErr(error, QStringLiteral("line %1: bad or repeated release line").arg(lineNo));
                    return std::nullopt;
                }
                m.releaseTag = tag;
                haveTag = true;
            }
            continue;
        }
        // "<64 hex><space><space|*><name>"
        const QByteArray hex = line.left(64);
        if (line.size() < 67 || !isHexDigest(hex) || line.at(64) != ' '
            || (line.at(65) != ' ' && line.at(65) != '*')) {
            setErr(error, QStringLiteral("line %1: not a sha256sum line").arg(lineNo));
            return std::nullopt;
        }
        const QString name = QString::fromUtf8(line.mid(66));
        if (name.isEmpty() || name.contains(QLatin1Char('/'))
            || name.contains(QLatin1Char('\\')) || name == QLatin1String("..")
            || name != name.trimmed()) {
            setErr(error, QStringLiteral("line %1: bad file name").arg(lineNo));
            return std::nullopt;
        }
        if (m.sha256.contains(name)) {
            setErr(error, QStringLiteral("line %1: %2 listed twice").arg(lineNo).arg(name));
            return std::nullopt;
        }
        m.sha256.insert(name, hex.toLower());
    }
    if (!haveTag) {
        setErr(error, QStringLiteral("manifest has no release line"));
        return std::nullopt;
    }
    return m;
}

std::optional<QByteArray> expectedAssetDigest(
    const QByteArray& manifest, const QByteArray& signature,
    const QString& releaseTag, const QString& assetName,
    const QList<QByteArray>& publicKeysRaw, QString* error)
{
    QString why;
    if (!verifyEd25519(manifest, signature, publicKeysRaw, &why)) {
        setErr(error, QStringLiteral("SHA256SUMS signature invalid: %1").arg(why));
        return std::nullopt;
    }
    const std::optional<Manifest> m = parseManifest(manifest, &why);
    if (!m) {
        setErr(error, QStringLiteral("SHA256SUMS unreadable: %1").arg(why));
        return std::nullopt;
    }
    if (m->releaseTag != releaseTag) {
        setErr(error, QStringLiteral("SHA256SUMS is for %1, not %2")
                          .arg(m->releaseTag, releaseTag));
        return std::nullopt;
    }
    const auto it = m->sha256.constFind(assetName);
    if (it == m->sha256.constEnd()) {
        setErr(error, QStringLiteral("SHA256SUMS does not list %1").arg(assetName));
        return std::nullopt;
    }
    return it.value();
}

QString parseCodesignTeamId(const QString& codesignOutput)
{
    static const QRegularExpression re(
        QStringLiteral("^TeamIdentifier=([A-Z0-9]{10})\\s*$"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = re.match(codesignOutput);
    return match.hasMatch() ? match.captured(1) : QString();
}

bool Sha256FileSink::open(const QString& finalPath, QString* error)
{
    m_file = std::make_unique<QSaveFile>(finalPath);
    m_hash.reset();
    m_bytes = 0;
    m_writeFailed = false;
    if (!m_file->open(QIODevice::WriteOnly)) {
        setErr(error, m_file->errorString());
        m_file.reset();
        return false;
    }
    return true;
}

bool Sha256FileSink::write(const QByteArray& chunk)
{
    if (!m_file || m_writeFailed) return false;
    if (chunk.isEmpty()) return true;
    if (m_file->write(chunk) != chunk.size()) {
        m_writeFailed = true;
        return false;
    }
    m_hash.addData(chunk);
    m_bytes += chunk.size();
    return true;
}

bool Sha256FileSink::finish(const QByteArray& expectedHex, QString* error)
{
    if (!m_file) {
        setErr(error, QStringLiteral("no download in progress"));
        return false;
    }
    if (m_writeFailed) {
        setErr(error, m_file->errorString());
        abort();
        return false;
    }
    const QByteArray actual = m_hash.result().toHex();
    if (!isHexDigest(expectedHex) || actual != expectedHex.toLower()) {
        setErr(error, QStringLiteral("SHA-256 mismatch: expected %1, got %2")
                          .arg(QString::fromLatin1(expectedHex.toLower()),
                               QString::fromLatin1(actual)));
        abort();
        return false;
    }
    if (!m_file->commit()) {
        setErr(error, m_file->errorString());
        m_file.reset();
        return false;
    }
    m_file.reset();
    return true;
}

void Sha256FileSink::abort()
{
    if (m_file) m_file->cancelWriting();
    m_file.reset();   // an uncommitted QSaveFile deletes its temp file
}

QString Sha256FileSink::errorString() const
{
    return m_file ? m_file->errorString() : QString();
}

QString pinnedMacTeamId()
{
#ifdef BSFCHAT_MACOS_TEAM_ID
    return QStringLiteral(BSFCHAT_MACOS_TEAM_ID).trimmed();
#else
    return QString();
#endif
}

QString macTeamMismatch(const QString& newTeam, const QString& pinned,
                        const QString& selfTeam)
{
    if (newTeam.isEmpty())
        return QStringLiteral("it is not signed by any developer team");
    if (!pinned.isEmpty()) {
        return newTeam == pinned
            ? QString()
            : QStringLiteral("it is signed by team \"%1\", expected %2").arg(newTeam, pinned);
    }
    if (selfTeam.isEmpty())
        return QStringLiteral("this build has no Team ID to check the update "
                              "against, so it cannot update itself");
    return newTeam == selfTeam
        ? QString()
        : QStringLiteral("it is signed by team \"%1\", not %2 like this build")
              .arg(newTeam, selfTeam);
}

// ---- InstallerLock / copyVerified (C2-3) --------------------------------

namespace {
bool digestMatches(const QByteArray& actualHex, const QByteArray& expectedHex,
                   QString* error)
{
    if (isHexDigest(expectedHex) && actualHex == expectedHex.toLower()) return true;
    setErr(error, QStringLiteral("the installer changed after it was verified "
                                 "(SHA-256 %1, expected %2)")
                      .arg(QString::fromLatin1(actualHex),
                           QString::fromLatin1(expectedHex.toLower())));
    return false;
}
} // namespace

InstallerLock::~InstallerLock()
{
    release();
}

#ifdef Q_OS_WIN
bool InstallerLock::acquire(const QString& path, const QByteArray& expectedHex,
                            QString* error)
{
    release();
    // No FILE_SHARE_WRITE, no FILE_SHARE_DELETE: from here until release()
    // the bytes hashed below are the bytes CreateProcess will map.
    const QString native = QDir::toNativeSeparators(path);
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setErr(error, QStringLiteral("couldn't lock the installer (error %1)")
                          .arg(static_cast<qulonglong>(GetLastError())));
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buf(1 << 16, Qt::Uninitialized);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            setErr(error, QStringLiteral("couldn't read the installer (error %1)")
                              .arg(static_cast<qulonglong>(GetLastError())));
            CloseHandle(h);
            return false;
        }
        if (got == 0) break;
        hash.addData(QByteArrayView(buf.constData(), static_cast<qsizetype>(got)));
    }
    if (!digestMatches(hash.result().toHex(), expectedHex, error)) {
        CloseHandle(h);
        return false;
    }
    m_handle = h;
    return true;
}

void InstallerLock::release()
{
    if (m_handle) CloseHandle(static_cast<HANDLE>(m_handle));
    m_handle = nullptr;
}

bool InstallerLock::held() const
{
    return m_handle != nullptr;
}
#else
bool InstallerLock::acquire(const QString& path, const QByteArray& expectedHex,
                            QString* error)
{
    release();
    auto f = std::make_unique<QFile>(path);
    if (!f->open(QIODevice::ReadOnly)) {
        setErr(error, f->errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(f.get())) {
        setErr(error, QStringLiteral("couldn't read the installer"));
        return false;
    }
    if (!digestMatches(hash.result().toHex(), expectedHex, error)) return false;
    m_file = std::move(f);
    return true;
}

void InstallerLock::release()
{
    m_file.reset();
}

bool InstallerLock::held() const
{
    return m_file != nullptr;
}
#endif

bool copyVerified(const QString& src, const QString& destPath,
                  const QByteArray& expectedHex, QString* error)
{
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        setErr(error, in.errorString());
        return false;
    }
    Sha256FileSink sink;
    if (!sink.open(destPath, error)) return false;
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(1 << 16);
        if (chunk.isEmpty() && in.error() != QFileDevice::NoError) {
            setErr(error, in.errorString());
            sink.abort();
            return false;
        }
        if (!sink.write(chunk)) {
            setErr(error, sink.errorString());
            sink.abort();
            return false;
        }
    }
    QString why;
    if (!sink.finish(expectedHex, &why)) {
        setErr(error, QStringLiteral("the installer changed after it was verified (%1)")
                          .arg(why));
        return false;
    }
    return true;
}

} // namespace bsfchat::updates

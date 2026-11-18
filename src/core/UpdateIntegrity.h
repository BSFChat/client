// Integrity checks for the desktop auto-updater (audit M-5).
//
// The Updater downloads an installer from GitHub and runs it — on Windows
// elevated and silently. TLS alone only proves the bytes came from GitHub,
// not that we published them: anyone holding a token that can edit release
// assets could swap the installer. So, before anything is run:
//
//   1. Every URL — the asset link from the API and every redirect hop — must
//      be https on a GitHub release host (isAllowedAssetUrl / ...RedirectUrl).
//   2. The release must carry SHA256SUMS and SHA256SUMS.sig. The signature is
//      Ed25519 over the exact SHA256SUMS bytes, made by the release CI with a
//      key whose public half is compiled in (ReleaseSigningKey.h).
//   3. SHA256SUMS names the release tag it belongs to (`# release: <tag>`),
//      so a correctly signed manifest from an OLDER release cannot be
//      replayed to push a downgrade.
//   4. The installer is hashed while it streams to disk (Sha256FileSink) and
//      only lands at its final path if the hash matches.
//
// Everything here is free of QNetworkAccessManager so it is unit-tested
// directly (tests/test_update_integrity.cpp). See docs/release-signing.md.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QCryptographicHash>
#include <QHash>
#include <QList>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <memory>
#include <optional>

namespace bsfchat::updates {

inline constexpr char kManifestAssetName[] = "SHA256SUMS";
inline constexpr char kSignatureAssetName[] = "SHA256SUMS.sig";
// SHA256SUMS for three assets is ~300 bytes; anything near this is not ours.
inline constexpr qint64 kMaxManifestBytes = 64 * 1024;
inline constexpr int kEd25519SignatureBytes = 64;
inline constexpr int kEd25519PublicKeyBytes = 32;

// The URL the GitHub API hands us for an asset. Strict: https, github.com,
// default port, no userinfo, and a path under the BSFChat/client release
// download tree.
bool isAllowedAssetUrl(const QUrl& url);

// A redirect hop (or the initial URL). https, default port, no userinfo,
// and one of the hosts GitHub serves release downloads from: github.com
// itself, release-assets.githubusercontent.com (current, verified
// 2026-11-15) and objects.githubusercontent.com (its predecessor).
bool isAllowedRedirectUrl(const QUrl& url);

// Ed25519 verification of `message` against any of `publicKeysRaw` (32-byte
// raw keys). `signature` is the raw 64-byte signature
// (`openssl pkeyutl -sign -rawin` output).
bool verifyEd25519(const QByteArray& message, const QByteArray& signature,
                   const QList<QByteArray>& publicKeysRaw,
                   QString* error = nullptr);

// The compiled-in release keys (ReleaseSigningKey.h), decoded.
QList<QByteArray> releaseSigningKeys();

struct Manifest {
    QString releaseTag;                 // from `# release: <tag>`
    QHash<QString, QByteArray> sha256;  // asset name -> lowercase hex digest
};

// Parse SHA256SUMS. Accepts `sha256sum` output ("<hex>  <name>" or
// "<hex> *<name>"), blank lines and `#` comments. Rejects — rather than
// skips — anything malformed, a name with a path separator, a duplicate
// name, a missing or duplicated `# release:` line, or an oversize body:
// a manifest that is not exactly what CI writes was not written by CI.
std::optional<Manifest> parseManifest(const QByteArray& body,
                                      QString* error = nullptr);

// The full check the Updater runs on the two small downloads: signature
// first (nothing in an unauthenticated body is trusted), then parse, then
// the tag binding, then look up `assetName`. Returns the expected digest
// (lowercase hex) or nullopt with `error` set.
std::optional<QByteArray> expectedAssetDigest(
    const QByteArray& manifest, const QByteArray& signature,
    const QString& releaseTag, const QString& assetName,
    const QList<QByteArray>& publicKeysRaw, QString* error = nullptr);

// `codesign -dv` writes "TeamIdentifier=ABCDE12345" (or "=not set") to
// stderr. Returns the team id, or "" if absent / not set / malformed.
QString parseCodesignTeamId(const QString& codesignOutput);

// The Apple Team ID release builds are pinned to (-DBSFCHAT_MACOS_TEAM_ID,
// set by CI), or "" for a build without one.
QString pinnedMacTeamId();

// Why a macOS update bundle signed by `newTeam` must NOT be installed, or ""
// if it may. `pinned` (the build-time Team ID) wins when set; otherwise the
// running bundle's own `selfTeam` is the reference, and with neither there
// is nothing to check the update against, so it is refused (C2-3).
QString macTeamMismatch(const QString& newTeam, const QString& pinned,
                        const QString& selfTeam);

// Streams a download to `finalPath` while hashing it. Nothing appears at
// `finalPath` unless finish() saw the expected digest; on any failure the
// partial file is discarded.
class Sha256FileSink {
public:
    bool open(const QString& finalPath, QString* error = nullptr);
    bool write(const QByteArray& chunk);      // false on a short write
    // Compares, then atomically commits. `expectedHex` is case-insensitive.
    bool finish(const QByteArray& expectedHex, QString* error = nullptr);
    void abort();
    qint64 bytesWritten() const { return m_bytes; }
    QString errorString() const;

private:
    std::unique_ptr<QSaveFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_bytes = 0;
    bool m_writeFailed = false;
};

// ---- Time of check vs time of use (C2-3) -------------------------------
//
// The verified installer sits in a user-writable cache between the download
// and "Restart to install" — minutes or days — and on Windows it then runs
// ELEVATED, unsigned, so UAC cannot show that it changed. So it is hashed
// again at the last moment, through the handle that pins it.

// Holds a file open so that it cannot change between its last hash and its
// launch. On Windows the handle is opened with FILE_SHARE_READ only: while it
// is held no other process can open the file for writing, rename it or
// delete it (and its directory cannot be renamed), but the installer can
// still be started and can read itself. POSIX has no mandatory share modes;
// there the lock only re-hashes (use copyVerified for a private copy).
class InstallerLock {
public:
    InstallerLock() = default;
    ~InstallerLock();
    InstallerLock(const InstallerLock&) = delete;
    InstallerLock& operator=(const InstallerLock&) = delete;

    // Opens `path`, hashes it through that handle and keeps it open only if
    // the digest matches `expectedHex` (case-insensitive).
    bool acquire(const QString& path, const QByteArray& expectedHex,
                 QString* error = nullptr);
    void release();
    bool held() const;

private:
#ifdef Q_OS_WIN
    void* m_handle = nullptr; // HANDLE
#else
    std::unique_ptr<QFile> m_file;
#endif
};

// Copy `src` to `destPath` hashing exactly the bytes written; `destPath` only
// appears if they match `expectedHex`. For a private, freshly verified copy
// (macOS attaches the DMG from one in a 0700 temp directory).
bool copyVerified(const QString& src, const QString& destPath,
                  const QByteArray& expectedHex, QString* error = nullptr);

} // namespace bsfchat::updates

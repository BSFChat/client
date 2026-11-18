// Updater integrity checks (audit M-5): URL pinning, SHA256SUMS parsing,
// Ed25519 manifest signatures and the streaming hash sink.
//
// Pure logic — no QNetworkAccessManager. Updater.cpp feeds these exactly
// what it downloads, so the decisions that stand between a GitHub asset and
// an elevated installer are all exercised here.
//
// Keys: most cases sign with a keypair generated inside the test. One case
// uses a signature made by the `openssl pkeyutl -sign -rawin` command the
// release CI runs (throwaway key, not the release key), so a mismatch
// between the CLI's output format and the client's verifier fails here
// rather than on every user's machine.

#include "core/ReleaseSigningKey.h"
#include "core/UpdateIntegrity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <openssl/evp.h>

using namespace bsfchat::updates;

namespace {

struct TestKey {
    QByteArray publicRaw;
    EVP_PKEY* pkey = nullptr;
    TestKey()
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (ctx && EVP_PKEY_keygen_init(ctx) == 1) EVP_PKEY_keygen(ctx, &pkey);
        EVP_PKEY_CTX_free(ctx);
        if (!pkey) return;
        size_t len = 32;
        publicRaw.resize(32);
        EVP_PKEY_get_raw_public_key(
            pkey, reinterpret_cast<unsigned char*>(publicRaw.data()), &len);
        publicRaw.resize(static_cast<qsizetype>(len));
    }
    ~TestKey() { EVP_PKEY_free(pkey); }
    TestKey(const TestKey&) = delete;
    TestKey& operator=(const TestKey&) = delete;

    QByteArray sign(const QByteArray& msg) const
    {
        QByteArray sig(64, '\0');
        size_t len = 64;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        const bool ok = ctx
            && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1
            && EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                              reinterpret_cast<const unsigned char*>(msg.constData()),
                              static_cast<size_t>(msg.size())) == 1;
        EVP_MD_CTX_free(ctx);
        return ok ? sig.left(static_cast<qsizetype>(len)) : QByteArray();
    }
};

QByteArray sha256Hex(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray manifestFor(const QString& tag, const QByteArray& payload,
                       const QByteArray& name = "BSFChat-Setup.exe")
{
    return "# release: " + tag.toUtf8() + "\n"
         + sha256Hex(payload) + "  " + name + "\n"
         + sha256Hex("other") + "  BSFChat-macOS.dmg\n";
}

} // namespace

class TestUpdateIntegrity : public QObject {
    Q_OBJECT

private slots:
    // ── URL pinning ──────────────────────────────────────────────────

    void assetUrl_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("allowed");
        const QString ok = QStringLiteral(
            "https://github.com/BSFChat/client/releases/download/v0.0.48/BSFChat-Setup.exe");
        QTest::newRow("real asset") << ok << true;
        QTest::newRow("sums") << QStringLiteral(
            "https://github.com/BSFChat/client/releases/download/v0.0.48-rc.19/SHA256SUMS") << true;
        QTest::newRow("http") << QString(ok).replace("https:", "http:") << false;
        QTest::newRow("other host") << QStringLiteral(
            "https://evil.example/BSFChat/client/releases/download/v1/x.exe") << false;
        QTest::newRow("lookalike host") << QStringLiteral(
            "https://github.com.evil.example/BSFChat/client/releases/download/v1/x.exe") << false;
        QTest::newRow("other repo") << QStringLiteral(
            "https://github.com/someone/client/releases/download/v1/x.exe") << false;
        QTest::newRow("userinfo") << QStringLiteral(
            "https://user@github.com/BSFChat/client/releases/download/v1/x.exe") << false;
        QTest::newRow("port") << QStringLiteral(
            "https://github.com:8443/BSFChat/client/releases/download/v1/x.exe") << false;
        QTest::newRow("dotdot") << QStringLiteral(
            "https://github.com/BSFChat/client/releases/download/../../../evil/x") << false;
        QTest::newRow("encoded slash") << QStringLiteral(
            "https://github.com/BSFChat/client/releases/download/v1%2F..%2F..%2Fx/y") << false;
        QTest::newRow("too deep") << QStringLiteral(
            "https://github.com/BSFChat/client/releases/download/v1/a/b") << false;
        QTest::newRow("empty") << QString() << false;
    }
    void assetUrl()
    {
        QFETCH(QString, url);
        QFETCH(bool, allowed);
        QCOMPARE(isAllowedAssetUrl(QUrl(url)), allowed);
    }

    void redirectUrl_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("release-assets") << QStringLiteral(
            "https://release-assets.githubusercontent.com/github-production-release-asset/1/abc?sp=r")
            << true;
        QTest::newRow("objects") << QStringLiteral(
            "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1") << true;
        QTest::newRow("github.com") << QStringLiteral(
            "https://github.com/BSFChat/client/releases/download/v1/x") << true;
        QTest::newRow("uppercase host") << QStringLiteral(
            "https://Release-Assets.GitHubUserContent.com/x") << true;
        QTest::newRow("http downgrade") << QStringLiteral(
            "http://release-assets.githubusercontent.com/x") << false;
        QTest::newRow("other cdn") << QStringLiteral("https://evil.example/x") << false;
        QTest::newRow("suffix trick") << QStringLiteral(
            "https://objects.githubusercontent.com.evil.example/x") << false;
        QTest::newRow("raw user content") << QStringLiteral(
            "https://raw.githubusercontent.com/x/y/z") << false;
        QTest::newRow("ftp") << QStringLiteral("ftp://github.com/x") << false;
    }
    void redirectUrl()
    {
        QFETCH(QString, url);
        QFETCH(bool, allowed);
        QCOMPARE(isAllowedRedirectUrl(QUrl(url)), allowed);
    }

    // ── Manifest parsing ─────────────────────────────────────────────

    void parsesSha256sumOutput()
    {
        const QByteArray body =
            "# release: v0.0.49\n"
            "87428fc522803d31065e7bce3cf03fe475096631e5e07bbd7a0fde60c4cf25c7  BSFChat-macOS.dmg\r\n"
            "0263829989B6FD954F72BAAF2FC64BC2E2F01D692D4DE72986EA808F6E99813F *BSFChat-Setup.exe\n"
            "\n"
            "# a comment\n";
        QString err;
        const auto m = parseManifest(body, &err);
        QVERIFY2(m.has_value(), qPrintable(err));
        QCOMPARE(m->releaseTag, QStringLiteral("v0.0.49"));
        QCOMPARE(m->sha256.size(), 2);
        QCOMPARE(m->sha256.value("BSFChat-macOS.dmg"),
                 QByteArray("87428fc522803d31065e7bce3cf03fe475096631e5e07bbd7a0fde60c4cf25c7"));
        // Normalised to lowercase so the comparison with toHex() is exact.
        QCOMPARE(m->sha256.value("BSFChat-Setup.exe"),
                 QByteArray("0263829989b6fd954f72baaf2fc64bc2e2f01d692d4de72986ea808f6e99813f"));
    }

    void rejectsMalformedManifests_data()
    {
        QTest::addColumn<QByteArray>("body");
        const QByteArray h(64, 'a');
        QTest::newRow("no release line") << QByteArray(h + "  a.exe\n");
        QTest::newRow("two release lines")
            << QByteArray("# release: v1\n# release: v2\n" + h + "  a.exe\n");
        QTest::newRow("empty release") << QByteArray("# release:   \n" + h + "  a.exe\n");
        QTest::newRow("short hash") << QByteArray("# release: v1\n" + h.left(63) + "  a.exe\n");
        QTest::newRow("non-hex") << QByteArray("# release: v1\n" + QByteArray(64, 'g') + "  a.exe\n");
        QTest::newRow("one space") << QByteArray("# release: v1\n" + h + " a.exe\n");
        QTest::newRow("no name") << QByteArray("# release: v1\n" + h + "  \n");
        QTest::newRow("path") << QByteArray("# release: v1\n" + h + "  dir/a.exe\n");
        QTest::newRow("backslash") << QByteArray("# release: v1\n" + h + "  ..\\a.exe\n");
        QTest::newRow("duplicate")
            << QByteArray("# release: v1\n" + h + "  a.exe\n" + QByteArray(64, 'b') + "  a.exe\n");
        QTest::newRow("garbage") << QByteArray("# release: v1\nhello world\n");
        QTest::newRow("too large")
            << QByteArray("# release: v1\n" + QByteArray(kMaxManifestBytes, '#'));
    }
    void rejectsMalformedManifests()
    {
        QFETCH(QByteArray, body);
        QString err;
        QVERIFY(!parseManifest(body, &err).has_value());
        QVERIFY(!err.isEmpty());
    }

    // ── Signatures ───────────────────────────────────────────────────

    void verifiesAGeneratedKeypair()
    {
        TestKey key;
        QVERIFY(key.pkey);
        const QByteArray msg = manifestFor("v1.0.0", "installer");
        const QByteArray sig = key.sign(msg);
        QCOMPARE(sig.size(), kEd25519SignatureBytes);
        QVERIFY(verifyEd25519(msg, sig, {key.publicRaw}));

        // One flipped byte anywhere — message or signature — fails.
        QByteArray badMsg = msg;
        badMsg[10] = badMsg[10] ^ 0x01;
        QVERIFY(!verifyEd25519(badMsg, sig, {key.publicRaw}));
        QByteArray badSig = sig;
        badSig[0] = badSig[0] ^ 0x01;
        QVERIFY(!verifyEd25519(msg, badSig, {key.publicRaw}));
        // Wrong key, truncated signature, no keys.
        TestKey other;
        QVERIFY(!verifyEd25519(msg, sig, {other.publicRaw}));
        QVERIFY(!verifyEd25519(msg, sig.left(63), {key.publicRaw}));
        QVERIFY(!verifyEd25519(msg, QByteArray(), {key.publicRaw}));
        QVERIFY(!verifyEd25519(msg, sig, {}));
        // Rotation: any listed key may sign.
        QVERIFY(verifyEd25519(msg, sig, {other.publicRaw, key.publicRaw}));
    }

    void acceptsTheOpensslCliFormatUsedByCi()
    {
        // Made with: openssl pkeyutl -sign -rawin -inkey t.pem -in SUMS -out SUMS.sig
        const QByteArray pub = QByteArray::fromHex(
            "8417886f9f32e531612c5598254aeb6572e02e8f4b9edf68c1459e63d2e34cda");
        const QByteArray sig = QByteArray::fromHex(
            "4f545736bc93c34f0408ff3eb78c59b95ea32dc82c187ecc4094cb5f53c31c14"
            "c044a8fec8639455c5361f7cb38fc274ddb2df4f7431bbf77a63d7c0c72d5809");
        const QByteArray sums =
            "# release: v1.2.3\n"
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824  BSFChat-macOS.dmg\n";
        QString err;
        const auto digest = expectedAssetDigest(sums, sig, "v1.2.3", "BSFChat-macOS.dmg",
                                                {pub}, &err);
        QVERIFY2(digest.has_value(), qPrintable(err));
        QCOMPARE(*digest, sha256Hex("hello"));
    }

    void embeddedReleaseKeysAreWellFormed()
    {
        const QList<QByteArray> keys = releaseSigningKeys();
        QVERIFY(!keys.isEmpty());
        for (const QByteArray& k : keys) QCOMPARE(k.size(), kEd25519PublicKeyBytes);
        // And they are not the test vector's throwaway key.
        QVERIFY(!keys.contains(QByteArray::fromHex(
            "8417886f9f32e531612c5598254aeb6572e02e8f4b9edf68c1459e63d2e34cda")));
    }

    void expectedDigestEnforcesEveryBinding()
    {
        TestKey key;
        const QByteArray payload = "the installer";
        const QByteArray sums = manifestFor("v2.0.0", payload);
        const QByteArray sig = key.sign(sums);
        QString err;

        auto ok = expectedAssetDigest(sums, sig, "v2.0.0", "BSFChat-Setup.exe",
                                      {key.publicRaw}, &err);
        QVERIFY2(ok.has_value(), qPrintable(err));
        QCOMPARE(*ok, sha256Hex(payload));

        // Signed, but for another release: a replayed old manifest.
        QVERIFY(!expectedAssetDigest(sums, sig, "v2.0.1", "BSFChat-Setup.exe",
                                     {key.publicRaw}, &err));
        QVERIFY(err.contains("v2.0.0"));
        // Signed, but doesn't list this platform's asset.
        QVERIFY(!expectedAssetDigest(sums, sig, "v2.0.0", "BSFChat-linux-x86_64.tar.gz",
                                     {key.publicRaw}, &err));
        // Missing signature (an RC published without the secret).
        QVERIFY(!expectedAssetDigest(sums, QByteArray(), "v2.0.0", "BSFChat-Setup.exe",
                                     {key.publicRaw}, &err));
        QVERIFY(err.contains("signature"));
        // Valid manifest, signature by an untrusted key.
        TestKey attacker;
        QVERIFY(!expectedAssetDigest(sums, attacker.sign(sums), "v2.0.0",
                                     "BSFChat-Setup.exe", {key.publicRaw}, &err));
        // Tampered line, original signature.
        QByteArray tampered = sums;
        tampered.replace(sha256Hex(payload), sha256Hex("evil"));
        QVERIFY(!expectedAssetDigest(tampered, sig, "v2.0.0", "BSFChat-Setup.exe",
                                     {key.publicRaw}, &err));
    }

    // ── Streaming hash sink ──────────────────────────────────────────

    void sinkCommitsOnMatch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("BSFChat-Setup.exe");
        const QByteArray a(100000, 'x'), b(12345, 'y');
        Sha256FileSink sink;
        QVERIFY(sink.open(path));
        QVERIFY(sink.write(a));
        QVERIFY(!QFile::exists(path));   // nothing at the final path mid-stream
        QVERIFY(sink.write(b));
        QCOMPARE(sink.bytesWritten(), qint64(a.size() + b.size()));
        QString err;
        QVERIFY2(sink.finish(sha256Hex(a + b).toUpper(), &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), a + b);
    }

    void sinkDiscardsOnMismatch()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("BSFChat-macOS.dmg");
        // A previous, good download already sits there; a tampered one
        // must neither replace it nor leave a temp file behind.
        {
            QFile old(path);
            QVERIFY(old.open(QIODevice::WriteOnly));
            old.write("previous");
        }
        Sha256FileSink sink;
        QVERIFY(sink.open(path));
        QVERIFY(sink.write("tampered bytes"));
        QString err;
        QVERIFY(!sink.finish(sha256Hex("genuine bytes"), &err));
        QVERIFY(err.contains("mismatch"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("previous"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1);
    }

    void sinkRejectsAMissingOrMalformedDigest()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("x.bin");
        Sha256FileSink sink;
        QVERIFY(sink.open(path));
        QVERIFY(sink.write("data"));
        QVERIFY(!sink.finish(QByteArray()));
        QVERIFY(!QFile::exists(path));
        QVERIFY(!sink.finish(sha256Hex("data")));   // already aborted
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    }

    void sinkAbortLeavesNothing()
    {
        QTemporaryDir dir;
        Sha256FileSink sink;
        QVERIFY(sink.open(dir.filePath("y.bin")));
        QVERIFY(sink.write("partial"));
        sink.abort();
        QVERIFY(!sink.write("more"));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    }

    // ── C2-3: re-hash at launch ──────────────────────────────────────

    static bool writeFile(const QString& path, const QByteArray& data)
    {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    }

    void aLockIsOnlyHeldOverTheVerifiedBytes()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("BSFChat-Setup.exe");
        QVERIFY(writeFile(path, "installer"));
        InstallerLock lock;
        QString why;
        QVERIFY2(lock.acquire(path, sha256Hex("installer").toUpper(), &why), qPrintable(why));
        QVERIFY(lock.held());
        lock.release();
        QVERIFY(!lock.held());

        // Swapped in the cache after the download was verified.
        QVERIFY(writeFile(path, "installer, but evil"));
        QVERIFY(!lock.acquire(path, sha256Hex("installer"), &why));
        QVERIFY(!lock.held());
        QVERIFY(why.contains("changed"));
        QVERIFY(!lock.acquire(dir.filePath("missing.exe"), sha256Hex("installer")));
        QVERIFY(!lock.acquire(path, QByteArray()));
    }

#ifdef Q_OS_WIN
    // While held, nobody can rewrite, rename or delete the installer.
    void aHeldLockKeepsTheInstallerFromChanging()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("BSFChat-Setup.exe");
        QVERIFY(writeFile(path, "installer"));
        InstallerLock lock;
        QVERIFY(lock.acquire(path, sha256Hex("installer")));
        QFile w(path);
        QVERIFY(!w.open(QIODevice::WriteOnly | QIODevice::Append));
        QVERIFY(!QFile::rename(path, dir.filePath("moved.exe")));
        QVERIFY(!QFile::remove(path));
        // ...but it can still be read (the installer reads itself).
        QFile r(path);
        QVERIFY(r.open(QIODevice::ReadOnly));
        QCOMPARE(r.readAll(), QByteArray("installer"));
        r.close();
        lock.release();
        QVERIFY(QFile::remove(path));
    }
#endif

    void aVerifiedCopyIsMadeOnlyOfTheVerifiedBytes()
    {
        QTemporaryDir dir;
        const QString src = dir.filePath("BSFChat-macOS.dmg");
        const QString dst = dir.filePath("private/BSFChat-macOS.dmg");
        QVERIFY(QDir(dir.path()).mkdir("private"));
        QByteArray big(300 * 1024, 'x'); // several chunks
        QVERIFY(writeFile(src, big));
        QString why;
        QVERIFY2(copyVerified(src, dst, sha256Hex(big), &why), qPrintable(why));
        QFile f(dst);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), big);
        f.close();
        QVERIFY(QFile::remove(dst));

        QVERIFY(writeFile(src, "tampered"));
        QVERIFY(!copyVerified(src, dst, sha256Hex(big), &why));
        QVERIFY(!QFile::exists(dst));
        QVERIFY(why.contains("changed"));
    }

    void macTeamIdRules()
    {
        // Pinned at build time: exactly that team.
        QVERIFY(macTeamMismatch("AB12CD34EF", "AB12CD34EF", {}).isEmpty());
        QVERIFY(!macTeamMismatch("ZZ99ZZ99ZZ", "AB12CD34EF", "ZZ99ZZ99ZZ").isEmpty());
        // Not pinned: the running bundle's own team.
        QVERIFY(macTeamMismatch("AB12CD34EF", {}, "AB12CD34EF").isEmpty());
        QVERIFY(!macTeamMismatch("ZZ99ZZ99ZZ", {}, "AB12CD34EF").isEmpty());
        // Neither: nothing to check against, refused.
        QVERIFY(!macTeamMismatch("AB12CD34EF", {}, {}).isEmpty());
        // An ad-hoc update bundle is never accepted.
        QVERIFY(!macTeamMismatch({}, "AB12CD34EF", {}).isEmpty());
        QVERIFY(!macTeamMismatch({}, {}, {}).isEmpty());
    }

    // ── macOS codesign output ────────────────────────────────────────

    void parsesCodesignTeamId()
    {
        const QString signedOut = QStringLiteral(
            "Executable=/Applications/bsfchat-app.app/Contents/MacOS/bsfchat-app\n"
            "Identifier=com.bsfchat.app\n"
            "Authority=Developer ID Application: Example (AB12CD34EF)\n"
            "TeamIdentifier=AB12CD34EF\n"
            "Sealed Resources version=2 rules=13 files=200\n");
        QCOMPARE(parseCodesignTeamId(signedOut), QStringLiteral("AB12CD34EF"));
        QCOMPARE(parseCodesignTeamId(QStringLiteral(
            "Signature=adhoc\nTeamIdentifier=not set\n")), QString());
        QCOMPARE(parseCodesignTeamId(QString()), QString());
        // Not anchored to a line start: something else's text, not ours.
        QCOMPARE(parseCodesignTeamId(QStringLiteral(
            "Info.plist=TeamIdentifier=AB12CD34EF\n")), QString());
    }
};

QTEST_MAIN(TestUpdateIntegrity)
#include "test_update_integrity.moc"

#pragma once

// What "open this attachment" may do with a file somebody else sent.
//
// The OS picks the program for a file by its extension — on Windows,
// ShellExecute on `x.bat` RUNS it. The cache file's extension used to be
// copied from the mxc path, which the sender controls (`mxc://s/id/a.bat`, or
// `#.bat`), so opening an "attachment" could execute code with one click.
//
// So:
//   * the cache extension comes only from kSafeTypes below, matched against
//     the sender's file name or MIME type — never from the URL. No match
//     means no extension, which every desktop treats as "ask what to open
//     it with" rather than "run it";
//   * programs and scripts are refused outright, by file name or MIME type;
//   * everything that is not plain image/video/audio needs the user's
//     confirmation, showing the sender's file name.

#include <QString>
#include <QStringList>

#include <algorithm>

namespace bsfchat::client::attachment {

enum class Kind {
    Media,    // image / video / audio we know: opened directly
    Document, // anything else that is not refused: confirm first
    Blocked,  // a program or script: never handed to the OS
};

struct SafeType {
    const char* ext;
    const char* mime;
    bool media;
};

// The only extensions a cached attachment can get. Deliberately absent: svg
// and html (they run script in a browser), and anything with macros.
inline constexpr SafeType kSafeTypes[] = {
    {"png", "image/png", true},       {"jpg", "image/jpeg", true},
    {"jpeg", "image/jpeg", true},     {"gif", "image/gif", true},
    {"webp", "image/webp", true},     {"bmp", "image/bmp", true},
    {"avif", "image/avif", true},     {"heic", "image/heic", true},
    {"mp4", "video/mp4", true},       {"m4v", "video/x-m4v", true},
    {"webm", "video/webm", true},     {"mov", "video/quicktime", true},
    {"mkv", "video/x-matroska", true}, {"mp3", "audio/mpeg", true},
    {"m4a", "audio/mp4", true},       {"ogg", "audio/ogg", true},
    {"oga", "audio/ogg", true},       {"opus", "audio/opus", true},
    {"wav", "audio/wav", true},       {"flac", "audio/flac", true},
    {"aac", "audio/aac", true},
    {"pdf", "application/pdf", false}, {"txt", "text/plain", false},
    {"log", "text/plain", false},     {"md", "text/markdown", false},
    {"csv", "text/csv", false},       {"json", "application/json", false},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", false},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", false},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation", false},
    {"odt", "application/vnd.oasis.opendocument.text", false},
    {"ods", "application/vnd.oasis.opendocument.spreadsheet", false},
    {"odp", "application/vnd.oasis.opendocument.presentation", false},
    {"zip", "application/zip", false},
};

// Extensions that are, or can launch, a program on some desktop.
inline const QStringList& blockedExtensions()
{
    static const QStringList k = {
        // Windows
        "exe", "msi", "msp", "mst", "msix", "msixbundle", "appx", "appxbundle",
        "bat", "cmd", "com", "scr", "pif", "cpl", "dll", "ocx", "sys", "drv",
        "ps1", "psm1", "psd1", "ps1xml", "psc1", "vbs", "vbe", "js", "jse",
        "wsf", "wsh", "ws", "wsc", "hta", "lnk", "url", "reg", "inf", "scf",
        "msc", "gadget", "application", "appref-ms", "settingcontent-ms",
        "library-ms", "search-ms", "chm", "hlp", "xll", "xlam", "docm", "dotm",
        "xlsm", "xltm", "pptm", "potm", "ppam", "sldm", "iso", "img", "vhd",
        "vhdx", "jar", "jnlp", "py", "pyw", "pyz", "pl", "rb", "php",
        // macOS
        "app", "command", "tool", "pkg", "mpkg", "dmg", "workflow", "action",
        "scpt", "scptd", "applescript", "terminal", "webloc", "inetloc",
        "fileloc", "dylib", "kext", "prefpane",
        // Linux / Unix
        "sh", "bash", "zsh", "csh", "ksh", "fish", "run", "bin", "elf", "so",
        "desktop", "appimage", "deb", "rpm", "snap", "flatpak", "flatpakref",
        // Android / iOS
        "apk", "apks", "xapk", "aab", "ipa",
        // Run script in a browser opened on a local file
        "html", "htm", "xhtml", "shtml", "mht", "mhtml", "svg", "svgz", "xml",
        "xsl", "xslt",
    };
    return k;
}

inline bool isBlockedMimeType(const QString& mime)
{
    static const QStringList k = {
        "application/x-msdownload", "application/x-msdos-program",
        "application/x-ms-installer", "application/x-msi", "application/x-dosexec",
        "application/vnd.microsoft.portable-executable", "application/x-ms-shortcut",
        "application/x-bat", "application/x-sh", "application/x-shellscript",
        "text/x-shellscript", "text/x-sh", "application/x-executable",
        "application/x-elf", "application/x-mach-binary", "application/x-sharedlib",
        "application/java-archive", "application/x-java-archive",
        "application/x-java-jnlp-file", "application/vnd.android.package-archive",
        "application/x-apple-diskimage", "application/x-iso9660-image",
        "application/hta", "application/javascript", "text/javascript",
        "application/x-javascript", "application/ecmascript", "text/vbscript",
        "application/x-powershell", "application/x-python-code", "text/x-python",
        "text/html", "application/xhtml+xml", "image/svg+xml",
        "application/x-desktop", "application/vnd.ms-cab-compressed",
    };
    return k.contains(mime.trimmed().toLower());
}

// The final extension of the sender's file name, lowercased, as Windows would
// read it: it drops trailing dots and spaces, so "invoice.exe. " is an .exe.
inline QString extensionOf(const QString& fileName)
{
    QString name = fileName;
    const qsizetype sep = std::max(name.lastIndexOf(QLatin1Char('/')),
                                   name.lastIndexOf(QLatin1Char('\\')));
    if (sep >= 0) name = name.mid(sep + 1);
    while (!name.isEmpty()
           && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))))
        name.chop(1);
    // An NTFS stream suffix ("a.exe:x") is still the .exe to the shell.
    const qsizetype colon = name.indexOf(QLatin1Char(':'));
    if (colon >= 0) name = name.left(colon);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot < 0 ? QString() : name.mid(dot + 1).trimmed().toLower();
}

inline const SafeType* safeTypeForExtension(const QString& ext)
{
    for (const auto& t : kSafeTypes)
        if (ext == QLatin1String(t.ext)) return &t;
    return nullptr;
}

inline const SafeType* safeTypeForMime(const QString& mime)
{
    const QString m = mime.trimmed().toLower();
    if (m.isEmpty()) return nullptr;
    for (const auto& t : kSafeTypes)
        if (m == QLatin1String(t.mime)) return &t;
    return nullptr;
}

inline Kind classify(const QString& fileName, const QString& mimeType)
{
    if (blockedExtensions().contains(extensionOf(fileName)) || isBlockedMimeType(mimeType))
        return Kind::Blocked;
    const SafeType* t = safeTypeForExtension(extensionOf(fileName));
    if (!t) t = safeTypeForMime(mimeType);
    return t && t->media ? Kind::Media : Kind::Document;
}

// ".png", ".pdf", … or "" — the extension the cached copy is written under.
// Only ever a member of kSafeTypes.
inline QString cacheExtension(const QString& fileName, const QString& mimeType)
{
    const SafeType* t = safeTypeForExtension(extensionOf(fileName));
    if (!t) t = safeTypeForMime(mimeType);
    return t ? QLatin1Char('.') + QLatin1String(t->ext) : QString();
}

} // namespace bsfchat::client::attachment

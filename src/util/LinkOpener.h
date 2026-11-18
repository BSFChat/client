// The one way QML opens a link outside the app. Exposed as the `linkOpener`
// context property; every call site that used Qt.openUrlExternally goes
// through here instead.
//
// What it adds over Qt.openUrlExternally:
//   * only http, https and mailto leave the app (util/LinkPolicy.h); anything
//     else — file:, smb:, UNC paths, javascript:, custom protocol handlers —
//     is refused and reported through `refused`, which the UI toasts;
//   * an http(s) link whose visible text names a different host, and every
//     mailto: link, is not opened until the user confirms:
//     `confirmationRequested` carries the real host in punycode and the full
//     percent-encoded URL as it will be opened (a mailto: with everything
//     but subject/body dropped), and the prompt answers with openConfirmed();
//   * it opens through bsfchat::openExternalUrl, so links work in the Linux
//     release tarball too (see util/ExternalBrowser.h) and a launch failure
//     is reported rather than silent.
//
// bsfchat:// links are routed by the call sites themselves, as before; one
// that reaches open() is ignored.
#pragma once

#include <QObject>
#include <QString>

class LinkOpener : public QObject {
    Q_OBJECT
public:
    explicit LinkOpener(QObject* parent = nullptr);

    // `contextHtml` is the rich text the link was clicked in, when there is
    // one; it is how the anchor's visible text is found, since a click
    // reports only the href.
    Q_INVOKABLE void open(const QString& href, const QString& contextHtml = QString());

    // The user said yes to `confirmationRequested`. The scheme is checked
    // again — this is still an untrusted string.
    Q_INVOKABLE void openConfirmed(const QString& href);

    // Open a directory on this machine (the log folder). Refuses anything
    // that is not an existing local directory, so it cannot be used to
    // launch a file.
    Q_INVOKABLE bool openLocalDirectory(const QString& path);

signals:
    void confirmationRequested(QString href, QString host, QString shownUrl);
    void refused(QString href, QString reason);
    void openFailed(QString href);
};

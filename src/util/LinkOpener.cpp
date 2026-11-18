#include "util/LinkOpener.h"

#include "util/ExternalBrowser.h"
#include "util/LinkPolicy.h"

#include <QDebug>
#include <QFileInfo>
#include <QUrl>

namespace links = bsfchat::client::links;

LinkOpener::LinkOpener(QObject* parent) : QObject(parent) {}

void LinkOpener::open(const QString& href, const QString& contextHtml)
{
    const auto d = links::decide(href, links::anchorTextsFor(contextHtml, href));
    switch (d.action) {
    case links::Action::Internal:
        qInfo() << "[links] ignoring an internal link that reached the external opener";
        return;
    case links::Action::Refuse:
        qWarning().noquote() << "[links] refused to open a link:" << d.reason;
        emit refused(href, d.reason);
        return;
    case links::Action::Confirm:
        emit confirmationRequested(href, d.host, d.shown);
        return;
    case links::Action::Open:
        if (!bsfchat::openExternalUrl(d.url)) emit openFailed(href);
        return;
    }
}

void LinkOpener::openConfirmed(const QString& href)
{
    // Without the anchor text this is Open for http(s) and Confirm for
    // mailto: — which the user just did confirm.
    const auto d = links::decide(href);
    if (d.action != links::Action::Open && d.action != links::Action::Confirm) {
        emit refused(href, d.reason);
        return;
    }
    if (!bsfchat::openExternalUrl(d.url)) emit openFailed(href);
}

bool LinkOpener::openLocalDirectory(const QString& path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isDir()) {
        qWarning() << "[links] not a local directory:" << path;
        return false;
    }
    return bsfchat::openExternalUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
}

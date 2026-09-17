#pragma once

// The one QtNetwork-shaped thing ServerDiscovery needs, kept out of
// ServerDiscovery.cpp so that file (and its test) links against Qt6::Core
// alone and can never accidentally touch the network.
//
// The timeout is the point. Discovery runs while the user watches a
// "Checking server capabilities…" line in the Add-server dialog, so a host
// that black-holes packets must fail in seconds, not after QNetworkReply's
// default of tens of seconds — and it must fail by *calling the callback*,
// because a dropped callback leaves that line spinning forever.

#include "net/ServerDiscovery.h"

#include <QObject>

namespace bsfchat {

// A FetchFn that GETs over HTTP(S), giving up after `timeoutMs`. The
// returned callable holds a QNetworkAccessManager parented to `owner`, so
// it must not outlive `owner`.
FetchFn networkFetch(QObject* owner, int timeoutMs);

// What the add-server flow allows itself to spend per request. Three
// requests worst case (well-known, versions, login), so the ceiling the
// user can feel is 3x this.
inline constexpr int kDiscoveryTimeoutMs = 6000;

} // namespace bsfchat

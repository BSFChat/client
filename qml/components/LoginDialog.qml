import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Dialog {
    id: dialog
    title: "Add Server"
    anchors.centerIn: parent
    // Clamp to viewport so the dialog doesn't overflow on narrow
    // phone screens. Min margin of 16dp on each side; max 400 on
    // large screens preserves the desktop look.
    width: Math.min(400, (parent ? parent.width : 400) - 32)
    // Also bound height so the dialog doesn't grow past the screen
    // when the software keyboard is up.
    height: Math.min(implicitHeight,
        (parent ? parent.height : 800) - 32)
    modal: true
    standardButtons: Dialog.NoButton

    property string errorMessage: ""
    property bool isConnecting: false
    property bool checkingFlows: false
    property bool oidcAvailable: false
    property string oidcProviderUrl: ""
    property bool passwordAvailable: true
    property bool oidcInProgress: false
    // When OIDC is available, password fields collapse behind a toggle
    // so OIDC is the obvious default. Click "Use password instead" to expand.
    property bool showPasswordFallback: false
    // True while we wait for the identity-first sync flow (browser OIDC +
    // /api/servers fetch + per-server auto-login).
    property bool identitySyncInProgress: false
    // Set when the app could not open a browser for a sign-in it has
    // already started (ServerManager::browserOpenFailed). Holds the
    // authorize URL, which the user opens by hand to finish: the attempt is
    // still live and the loopback callback still listening, so a link
    // pasted into any browser completes the sign-in normally.
    //
    // It cannot be retyped or rebuilt — it carries this attempt's PKCE
    // challenge, its CSRF state and the port the callback comes back to —
    // so showing it, selectable and copyable, IS the recovery.
    property string manualAuthUrl: ""

    // --- Which of the dialog's four screens is up.
    //
    //   "choose"    two buttons: sign in with a BSFChat ID, or join a
    //               server by its address. The opening screen.
    //   "address"   type an address, probe it, sign in to it.
    //   "signedIn"  who we just signed in as, and a way to say "not me".
    //   "noServers" signed in fine; the account has joined nothing yet.
    //
    // This replaced `showManualServer`, a bool that started false and hid
    // the entire address flow behind a text link reading "Add a specific
    // server". On a phone that link was the only route to the product's
    // central act — BSFChat is self-hosted, so joining somebody else's
    // server IS the flow — and the owner of the product had to be told
    // where it was. A store reviewer handed "sign in to uat.bsfchat.com"
    // would not have found it. The two paths are now two buttons of equal
    // weight on the screen the dialog opens on.
    property string mode: "choose"

    // Servers THIS sign-in added, so "not you?" can undo exactly what just
    // happened and nothing else. Anything already in the sidebar when the
    // dialog opened is somebody's earlier decision and is left alone.
    property var joinedUrls: []
    // Set while an address-mode connect is in flight, so the loginSuccess
    // that answers it lands on the confirmation screen while the dozen
    // that arrive during an identity sync do not each re-enter it.
    property bool awaitingSingleJoin: false
    // Shown on the chooser after "not you?": the local session is gone but
    // the browser's is not, and the next attempt will sail straight back in
    // as the same person unless they know that.
    property bool browserSessionNote: false

    // --- What the last probe of the typed URL found (see
    // ServerManager::serverProbed). Before a probe lands we know nothing,
    // and `probed` being false is what keeps the password/register form off
    // screen until the server has told us it accepts passwords. The old
    // dialog showed that form by default and treated any probe failure as
    // "password-only", so a URL that was not a server at all — the product
    // domain, a typo, a web server — got an invitation to register.
    property bool probed: false
    // The homeserver we will actually talk to. Differs from what was typed
    // when .well-known redirected us; shown either way so the user learns
    // the real address of the server they just joined.
    property string resolvedUrl: ""
    // "oidc" | "password" | "both" | "no_supported_flow" | "unreachable" |
    // "not_a_server". Mirrors bsfchat::loginFlowKindName.
    property string probeOutcome: ""
    // Non-empty only when discovery had to fall back from a well-known file
    // that named a homeserver which did not answer.
    property string probeNote: ""
    property bool probeRedirected: false

    // Open straight onto address entry. Both shells expose this as
    // Window.window.openJoinByAddress() so a shared component can offer
    // "join a server by address" as a first-class action without the user
    // having to find it inside the dialog first.
    function openAtAddress() {
        dialog.open();
        dialog.mode = "address";
        urlField.forceActiveFocus();
    }

    function probeFailed() {
        return dialog.probeOutcome === "not_a_server"
            || dialog.probeOutcome === "unreachable"
            || dialog.probeOutcome === "no_supported_flow";
    }

    function probeSummary() {
        if (dialog.probeOutcome === "not_a_server")
            return "No BSFChat server found at " + dialog.resolvedUrl;
        if (dialog.probeOutcome === "unreachable")
            return "Could not reach " + dialog.resolvedUrl
                 + " — check the address, or try again in a moment.";
        if (dialog.probeOutcome === "no_supported_flow")
            return dialog.resolvedUrl + " offers no sign-in method this app supports.";
        if (dialog.probeRedirected)
            return "Found server at " + dialog.resolvedUrl;
        return "Server: " + dialog.resolvedUrl;
    }

    // What the connect buttons send. Prefer the resolved homeserver so the
    // saved server entry is the real address; fall back to the raw text if
    // the user somehow gets here before a probe answered (ServerManager
    // resolves again on its side either way).
    function targetUrl() {
        return dialog.resolvedUrl !== "" ? dialog.resolvedUrl : urlField.text.trim();
    }

    // Every fresh probe starts from "we know nothing" — a stale outcome
    // from the previous URL must never decide what this URL is offered.
    function beginCheck(url) {
        dialog.checkingFlows = true;
        dialog.probed = false;
        dialog.oidcAvailable = false;
        dialog.passwordAvailable = false;
        dialog.showPasswordFallback = false;
        dialog.resolvedUrl = "";
        dialog.probeOutcome = "";
        dialog.probeNote = "";
        dialog.probeRedirected = false;
        serverManager.checkLoginFlows(url);
    }

    // The roster row serving `url`, or null. ServerListModel carries no
    // user id, so the live ServerConnection is the only place the @user:host
    // this phone just became is written down.
    function connectionFor(url) {
        if (!serverManager || !serverManager.servers) return null;
        var n = serverManager.servers.rowCount();
        for (var i = 0; i < n; ++i) {
            var c = serverManager.connectionAt(i);
            if (c && c.serverUrl === url) return c;
        }
        return null;
    }

    function rosterIndexOf(url) {
        if (!serverManager || !serverManager.servers) return -1;
        var n = serverManager.servers.rowCount();
        for (var i = 0; i < n; ++i) {
            var c = serverManager.connectionAt(i);
            if (c && c.serverUrl === url) return i;
        }
        return -1;
    }

    // The name to put in front of the user on the confirmation screen.
    //
    // Prefer the homeserver's own @user:host when we joined exactly one
    // server: that is the handle the other people in the room will see.
    // Otherwise the identity provider's account, which ServerManager reads
    // out of the id_token and therefore knows even when no homeserver has
    // answered yet — the case that matters, because the whole reason this
    // screen exists is a browser session completing the flow in silence.
    function signedInAs() {
        if (dialog.joinedUrls.length === 1) {
            var c = dialog.connectionFor(dialog.joinedUrls[0]);
            if (c && c.userId !== "") return c.userId;
        }
        if (serverManager.identityAccountName !== "")
            return serverManager.identityAccountName;
        return serverManager.identityAccountId;
    }

    // The quieter second line: whichever of the two identifiers signedInAs()
    // did not use. Empty when there is nothing more to add.
    function signedInDetail() {
        var primary = dialog.signedInAs();
        if (serverManager.identityAccountId !== ""
            && serverManager.identityAccountId !== primary) {
            return serverManager.identityAccountName !== ""
                   && serverManager.identityAccountName !== primary
                ? serverManager.identityAccountName
                  + " · " + serverManager.identityAccountId
                : serverManager.identityAccountId;
        }
        return "";
    }

    function joinedSummary() {
        if (dialog.joinedUrls.length === 0) return "";
        if (dialog.joinedUrls.length === 1)
            return "Joined " + dialog.joinedUrls[0];
        return "Restored " + dialog.joinedUrls.length + " servers.";
    }

    // "Not you?" — undo this sign-in.
    //
    // It cannot undo the half that caused the confusion: the session lives
    // in the system browser, and asking the provider to re-prompt means
    // sending `prompt=login` on the authorize request, which belongs to the
    // OIDC flow. So it does what it can honestly do — drop the servers this
    // sign-in added and the identity session behind them — and then says,
    // in words, what the user has to do themselves.
    function notMe() {
        for (var i = dialog.joinedUrls.length - 1; i >= 0; --i) {
            var idx = dialog.rosterIndexOf(dialog.joinedUrls[i]);
            if (idx >= 0) serverManager.removeServer(idx);
        }
        dialog.joinedUrls = [];
        serverManager.forgetIdentitySession();
        dialog.browserSessionNote = true;
        dialog.errorMessage = "";
        dialog.mode = "choose";
    }

    // Host only, for the "sign out in your browser" line — the full URL
    // makes that sentence unreadable on a phone.
    function identityHost() {
        var u = identityUrlField.text.trim();
        return u.replace(/^https?:\/\//, "").replace(/\/.*$/, "");
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    header: Rectangle {
        color: "transparent"
        height: 64
        Text {
            anchors.centerIn: parent
            text: dialog.mode === "signedIn" ? "Signed in"
                : dialog.mode === "noServers" ? "Almost there"
                : dialog.mode === "address" ? "Join a server"
                : "Add a server"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xl
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.xl
            color: Theme.fg0
        }
    }

    Connections {
        target: serverManager
        function onLoginFlowsChecked(url, oidcAvail, providerUrl, passwordAvail) {
            dialog.checkingFlows = false;
            dialog.oidcAvailable = oidcAvail;
            dialog.oidcProviderUrl = providerUrl;
            dialog.passwordAvailable = passwordAvail;
        }
        // The detail loginFlowsChecked has no room for: which of the six
        // outcomes happened, and where discovery decided the server lives.
        function onServerProbed(requestedUrl, resolvedUrl, outcome, providerUrl, redirected, note) {
            dialog.checkingFlows = false;
            dialog.probed = true;
            dialog.resolvedUrl = resolvedUrl;
            dialog.probeOutcome = outcome;
            dialog.probeRedirected = redirected;
            dialog.probeNote = note;
        }
        function onLoginSuccess(serverUrl) {
            dialog.oidcInProgress = false;
            dialog.isConnecting = false;
            dialog.errorMessage = "";
            // The join the user is watching: confirm it by name instead of
            // vanishing. Everything else — a server arriving late in an
            // identity sync, a background re-auth — leaves the screen alone.
            if (dialog.awaitingSingleJoin) {
                dialog.awaitingSingleJoin = false;
                dialog.joinedUrls = [serverUrl];
                dialog.mode = "signedIn";
                return;
            }
            if (dialog.mode === "signedIn" || dialog.mode === "noServers") return;
            dialog.close();
        }
        function onLoginError(serverUrl, error) {
            dialog.oidcInProgress = false;
            dialog.isConnecting = false;
            dialog.awaitingSingleJoin = false;
            // THE inline surface for a login failure (D-H5 / U-M13). This
            // used to be set from main.qml's own Connections on the same
            // signal, while a third handler toasted it — so one failure
            // produced up to three messages. The dialog owns the inline
            // copy; the shell only toasts when the dialog is not up.
            dialog.errorMessage = error;
        }
        function onIdentityLoginComplete(serverUrls) {
            dialog.identitySyncInProgress = false;
            dialog.joinedUrls = serverUrls;
            // Deliberately NOT close(). If the system browser already held
            // a session the entire flow just completed without showing the
            // user anything, and closing here is what let the owner sign in
            // as himself while expecting a demo account. The per-server
            // logins carry on behind this screen; "Continue" dismisses it.
            dialog.mode = "signedIn";
        }
        // Sign-in worked and the account has joined nothing. Not an error:
        // it used to arrive on the failure channel reading "Identity login
        // failed: your account isn't a member of any server yet", which is
        // both wrong and a dead end.
        function onIdentityHasNoServers() {
            dialog.identitySyncInProgress = false;
            dialog.joinedUrls = [];
            dialog.errorMessage = "";
            dialog.mode = "noServers";
        }
        function onIdentityLoginFailed(error) {
            dialog.identitySyncInProgress = false;
            dialog.errorMessage = "Identity login failed: " + error;
        }
        // Neither spinner is cleared: the sign-in really is still in
        // progress, waiting for the browser the user is about to open.
        function onBrowserOpenFailed(authUrl) {
            dialog.manualAuthUrl = authUrl;
        }
    }

    // Every re-open starts clean. Without this a failure that arrived
    // after the dialog was dismissed — or one left over from a previous
    // attempt at a different server — greeted the user the next time
    // they opened it (D-H5).
    onAboutToShow: {
        dialog.mode = "choose";
        dialog.errorMessage = "";
        dialog.isConnecting = false;
        dialog.oidcInProgress = false;
        dialog.identitySyncInProgress = false;
        dialog.manualAuthUrl = "";
        dialog.checkingFlows = false;
        dialog.awaitingSingleJoin = false;
        dialog.joinedUrls = [];
        dialog.probed = false;
        dialog.resolvedUrl = "";
        dialog.probeOutcome = "";
        dialog.probeNote = "";
        dialog.probeRedirected = false;
    }

    onClosed: {
        dialog.isConnecting = false;
        dialog.oidcInProgress = false;
        dialog.errorMessage = "";
        dialog.checkingFlows = false;
        dialog.oidcAvailable = false;
        dialog.oidcProviderUrl = "";
        dialog.passwordAvailable = true;
        dialog.showPasswordFallback = false;
        dialog.mode = "choose";
        dialog.awaitingSingleJoin = false;
        dialog.joinedUrls = [];
        dialog.browserSessionNote = false;
        dialog.identitySyncInProgress = false;
        dialog.manualAuthUrl = "";
        dialog.probed = false;
        dialog.resolvedUrl = "";
        dialog.probeOutcome = "";
        dialog.probeNote = "";
        dialog.probeRedirected = false;
    }

    // The dialog's height is clamped to the window, and main.qml sets the
    // window's own minimum to 500 px — but the sign-in form is taller than
    // that. With a bare ColumnLayout as contentItem the excess was simply
    // clipped: at the minimum window size the password fields and the Sign In
    // button were below the cut and unreachable, so the app could not be
    // signed into at a size it lets you resize to. It only scrolls when it has
    // to; at any normal window size nothing moves and nothing looks different.
    contentItem: Flickable {
        id: contentFlick
        implicitWidth: loginForm.implicitWidth
        implicitHeight: loginForm.implicitHeight
        contentWidth: width
        contentHeight: loginForm.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThemedScrollBar {}

        ColumnLayout {
            id: loginForm
            width: contentFlick.width
            spacing: Theme.sp.s5

            // ── Chooser ──────────────────────────────────────────────
            //
            // Two buttons, same size, same screen. Which one a person
            // wants depends on something they already know — whether they
            // have a BSFChat ID, or an address somebody gave them — so
            // neither can be "advanced".
            Text {
                visible: dialog.mode === "choose"
                text: "BSFChat servers are self-hosted. Sign in to restore the "
                    + "servers you've already joined, or join a new one by its address."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                color: Theme.fg1
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }

            Button {
                id: identityButton
                visible: dialog.mode === "choose"
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s3
                enabled: !dialog.identitySyncInProgress && !dialog.isConnecting && !dialog.oidcInProgress
                contentItem: Text {
                    text: dialog.identitySyncInProgress ? "Waiting for browser login…"
                                                        : "Sign in with BSFChat ID"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.md
                    color: parent.enabled ? Theme.onAccent : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: identityButton.enabled
                           ? (identityButton.hovered ? Theme.accentDim : Theme.accent)
                           : Theme.bg2
                    radius: Theme.r2
                    implicitHeight: 48
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: {
                    dialog.errorMessage = "";
                    dialog.browserSessionNote = false;
                    dialog.manualAuthUrl = "";
                    dialog.identitySyncInProgress = true;
                    serverManager.loginWithIdentityAndSync(identityUrlField.text.trim());
                }
            }

            // The other half of the choice. An outline rather than a second
            // filled button — equal footprint, equal reachability, but the
            // ID path stays the recommended one for somebody who has an ID.
            Button {
                id: joinByAddressButton
                visible: dialog.mode === "choose"
                Layout.fillWidth: true
                enabled: !dialog.identitySyncInProgress && !dialog.isConnecting && !dialog.oidcInProgress
                contentItem: Text {
                    text: "Join a server by address"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.md
                    color: joinByAddressButton.enabled ? Theme.fg0 : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: joinByAddressButton.hovered && joinByAddressButton.enabled
                           ? Theme.bg3 : "transparent"
                    border.color: joinByAddressButton.enabled ? Theme.accent : Theme.line
                    border.width: 1
                    radius: Theme.r2
                    implicitHeight: 48
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: {
                    dialog.errorMessage = "";
                    dialog.browserSessionNote = false;
                    dialog.mode = "address";
                    urlField.forceActiveFocus();
                }
            }

            // What "not you?" leaves behind. The local session is gone; the
            // browser's is not, and without this line the next attempt looks
            // broken — it signs straight back in as the same person.
            Text {
                visible: dialog.mode === "choose" && dialog.browserSessionNote
                Layout.fillWidth: true
                text: "Signed out on this device. Your browser is still signed in to "
                    + dialog.identityHost()
                    + " — sign out there first, or the same account will be used again."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg2
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }

            // Identity URL — editable for self-hosters, default to hosted service.
            RowLayout {
                Layout.fillWidth: true
                visible: dialog.mode === "choose"
                spacing: Theme.sp.s3

                Text {
                    text: "Identity server"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                }

                TextField {
                    id: identityUrlField
                    Layout.fillWidth: true
                    text: "https://id.bsfchat.com"
                    placeholderText: "https://id.bsfchat.com"
                    placeholderTextColor: Theme.fg3
                    color: Theme.fg0
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.sm
                    enabled: !dialog.identitySyncInProgress && !dialog.isConnecting && !dialog.oidcInProgress
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: identityUrlField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                    }
                    leftPadding: Theme.sp.s4
                    rightPadding: Theme.sp.s4
                    topPadding: Theme.sp.s3
                    bottomPadding: Theme.sp.s3
                }
            }

            // ── Signed in / no servers: who, and what next ───────────
            // Hidden rather than filled with a guess when we have no name
            // for the account: every real path supplies one (the identity
            // provider's `sub` always, a homeserver's @user:host always),
            // and a caption over a placeholder would be the same silence
            // this screen exists to end, wearing a label.
            Text {
                visible: (dialog.mode === "signedIn" || dialog.mode === "noServers")
                         && dialog.signedInAs() !== ""
                Layout.fillWidth: true
                text: "Signed in as"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                visible: (dialog.mode === "signedIn" || dialog.mode === "noServers")
                         && dialog.signedInAs() !== ""
                Layout.fillWidth: true
                text: dialog.signedInAs()
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.lg
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
                horizontalAlignment: Text.AlignHCenter
                // Wrap rather than elide: a long @user:homeserver.example
                // matters most in the middle, which is exactly what an
                // elide eats, and this screen exists to be read.
                wrapMode: Text.WrapAnywhere
            }

            Text {
                visible: (dialog.mode === "signedIn" || dialog.mode === "noServers")
                         && dialog.signedInDetail() !== ""
                Layout.fillWidth: true
                Layout.topMargin: -Theme.sp.s3
                text: dialog.signedInDetail()
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            Text {
                visible: dialog.mode === "signedIn" && dialog.joinedSummary() !== ""
                Layout.fillWidth: true
                text: dialog.joinedSummary()
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            // The dead end, answered. An identity account that belongs to no
            // server used to land on "Identity login failed: …" with the only
            // route onward hidden behind a disclosure link.
            Text {
                visible: dialog.mode === "noServers"
                Layout.fillWidth: true
                text: "This account hasn't joined a BSFChat server yet, so there's "
                    + "nothing to restore.\n\nBSFChat servers are run by the people "
                    + "who use them. Ask whoever runs yours for its address — "
                    + "something like chat.example.com — and join it below."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            Button {
                id: continueButton
                visible: dialog.mode === "signedIn"
                Layout.fillWidth: true
                contentItem: Text {
                    text: "Continue"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: continueButton.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitHeight: 48
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: dialog.close()
            }

            Button {
                id: noServersJoinButton
                visible: dialog.mode === "noServers"
                Layout.fillWidth: true
                contentItem: Text {
                    text: "Join a server by address"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: noServersJoinButton.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitHeight: 48
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: {
                    dialog.mode = "address";
                    urlField.forceActiveFocus();
                }
            }

            // "Not you?" — the whole point of the confirmation screen. A
            // browser that already holds a provider session completes the
            // OIDC round trip without showing anything, so this is the only
            // moment the wrong account is catchable.
            Button {
                id: notYouButton
                visible: dialog.mode === "signedIn" || dialog.mode === "noServers"
                Layout.fillWidth: true
                contentItem: Text {
                    text: "Not you? Sign out and use a different account"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.medium
                    color: notYouButton.hovered ? Theme.accentDim : Theme.accent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.Wrap
                }
                background: Rectangle {
                    color: "transparent"
                    implicitHeight: Theme.touchTarget
                }
                onClicked: dialog.notMe()
            }

            // ── Address entry ────────────────────────────────────────
            Button {
                id: backButton
                visible: dialog.mode === "address"
                Layout.alignment: Qt.AlignLeft
                contentItem: Text {
                    text: "‹ Back"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: backButton.hovered ? Theme.fg1 : Theme.fg3
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: "transparent"
                    implicitHeight: Theme.touchTarget
                    implicitWidth: 88
                }
                onClicked: {
                    dialog.errorMessage = "";
                    // Walking back out of a join in flight must not leave
                    // the flag armed, or the success that lands afterwards
                    // drags the user onto a confirmation screen for a
                    // server they just decided against.
                    dialog.awaitingSingleJoin = false;
                    dialog.mode = "choose";
                }
            }

            // Server URL
            ColumnLayout {
                spacing: Theme.sp.s1
                Layout.fillWidth: true
                visible: dialog.mode === "address"

                Text {
                    text: "SERVER ADDRESS"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s1

                    TextField {
                        id: urlField
                        Layout.fillWidth: true
                        placeholderText: "chat.example.com"
                        placeholderTextColor: Theme.fg2
                        color: Theme.fg0
                        font.pixelSize: Theme.fontSize.md
                        // A server address is a hostname. Autocapitalising it
                        // and offering to correct the spelling of "bsfchat"
                        // is how a phone turns a correct address into a
                        // "No BSFChat server found at …".
                        inputMethodHints: Qt.ImhUrlCharactersOnly
                                        | Qt.ImhNoAutoUppercase
                                        | Qt.ImhNoPredictiveText
                        enabled: !dialog.isConnecting && !dialog.oidcInProgress
                        background: Rectangle {
                            color: Theme.bg0
                            radius: Theme.r2
                            border.color: urlField.activeFocus ? Theme.accent : Theme.line
                            border.width: 1
                            implicitHeight: Theme.touchTarget
                        }
                        padding: Theme.sp.s3

                        onEditingFinished: {
                            if (urlField.text.trim() !== "")
                                dialog.beginCheck(urlField.text.trim());
                        }
                        Keys.onReturnPressed: {
                            if (urlField.text.trim() !== "")
                                dialog.beginCheck(urlField.text.trim());
                        }
                    }

                    // Ghost "Check" — probes the server for available login
                    // flows (OIDC / password) without committing to a connect.
                    Button {
                        id: checkButton
                        enabled: urlField.text.trim() !== "" && !dialog.isConnecting && !dialog.oidcInProgress && !dialog.checkingFlows
                        contentItem: Text {
                            text: "Check"
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            font.weight: Theme.fontWeight.medium
                            color: checkButton.enabled ? Theme.fg1 : Theme.fg3
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: checkButton.hovered && checkButton.enabled ? Theme.bg3 : "transparent"
                            border.color: Theme.line
                            border.width: 1
                            radius: Theme.r2
                            implicitWidth: 80
                            // Sits beside urlField, so it inherits the same
                            // floor: a 36-px control next to a 44-px field is
                            // a miss on a phone and a ragged row everywhere.
                            implicitHeight: Theme.touchTarget
                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        }
                        onClicked: {
                            dialog.errorMessage = "";
                            dialog.beginCheck(urlField.text.trim());
                        }
                    }
                }
            }

            // Checking indicator
            Text {
                text: "Checking server capabilities..."
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                visible: dialog.checkingFlows && dialog.mode === "address"
                Layout.alignment: Qt.AlignHCenter
            }

            // What we found. THE surface for "that is not a server" — the case
            // the old dialog answered with a registration form. Also names the
            // homeserver we resolved to, so someone who typed the product
            // domain learns the address their client is actually using.
            Text {
                Layout.fillWidth: true
                visible: dialog.mode === "address" && dialog.probed && !dialog.checkingFlows
                text: dialog.probeSummary()
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: dialog.probeFailed() ? Theme.danger : Theme.fg2
                wrapMode: Text.Wrap
            }

            // Only set when a well-known file named a homeserver that did not
            // answer and we fell back to the typed URL. Silence otherwise:
            // having no well-known file at all is the normal case.
            Text {
                Layout.fillWidth: true
                visible: dialog.mode === "address" && dialog.probeNote !== "" && !dialog.checkingFlows
                text: dialog.probeNote
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
                wrapMode: Text.Wrap
            }

            // OIDC login button
            Button {
                id: oidcButton
                Layout.fillWidth: true
                visible: dialog.mode === "address" && dialog.oidcAvailable && !dialog.checkingFlows
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                contentItem: Text {
                    text: "Sign in with BSFChat ID"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: oidcButton.enabled ? Theme.onAccent : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: !oidcButton.enabled ? Theme.bg2
                         : (oidcButton.hovered ? Theme.accentDim : Theme.accent)
                    radius: Theme.r2
                    implicitHeight: Theme.touchTarget
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: {
                    dialog.errorMessage = "";
                    dialog.manualAuthUrl = "";
                    dialog.oidcInProgress = true;
                    dialog.awaitingSingleJoin = true;
                    serverManager.addServerWithOidc(dialog.targetUrl());
                }
            }

            // Identity-only servers (the official one) have no register form to
            // fall back to, and the button above gives no clue that it will
            // make an account as well as use one. Say so, in one line, rather
            // than leaving "where do I sign up?" as the user's problem.
            Text {
                Layout.fillWidth: true
                Layout.topMargin: -Theme.sp.s1
                visible: dialog.mode === "address" && dialog.probed && dialog.oidcAvailable
                         && !dialog.passwordAvailable && !dialog.checkingFlows
                text: "No account needed — one is created the first time you sign in."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            // When OIDC is available, collapse password behind a link.
            // When OIDC is NOT available, show password fields directly.
            Button {
                id: passwordToggle
                Layout.fillWidth: true
                Layout.topMargin: -Theme.sp.s1
                visible: dialog.mode === "address" && dialog.probed && dialog.oidcAvailable
                         && dialog.passwordAvailable && !dialog.checkingFlows
                contentItem: Text {
                    text: dialog.showPasswordFallback ? "Hide password login"
                                                      : "Use password instead"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.accent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: "transparent"
                    implicitHeight: Theme.touchTarget
                }
                onClicked: dialog.showPasswordFallback = !dialog.showPasswordFallback
            }

            // Username (only when password auth is relevant and not collapsed)
            ColumnLayout {
                spacing: Theme.sp.s1
                Layout.fillWidth: true
                visible: dialog.mode === "address" && dialog.probed && dialog.passwordAvailable
                         && !dialog.checkingFlows
                         && (!dialog.oidcAvailable || dialog.showPasswordFallback)

                Text {
                    text: "USERNAME"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                }

                TextField {
                    id: usernameField
                    Layout.fillWidth: true
                    placeholderText: "Enter username"
                    placeholderTextColor: Theme.fg2
                    color: Theme.fg0
                    font.pixelSize: Theme.fontSize.md
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    enabled: !dialog.isConnecting && !dialog.oidcInProgress
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: usernameField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                        implicitHeight: Theme.touchTarget
                    }
                    padding: Theme.sp.s3
                }
            }

            // Password
            ColumnLayout {
                spacing: Theme.sp.s1
                Layout.fillWidth: true
                visible: dialog.mode === "address" && dialog.probed && dialog.passwordAvailable
                         && !dialog.checkingFlows
                         && (!dialog.oidcAvailable || dialog.showPasswordFallback)

                Text {
                    text: "PASSWORD"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                }

                TextField {
                    id: passwordField
                    Layout.fillWidth: true
                    placeholderText: "Enter password"
                    placeholderTextColor: Theme.fg2
                    color: Theme.fg0
                    font.pixelSize: Theme.fontSize.md
                    echoMode: TextInput.Password
                    enabled: !dialog.isConnecting && !dialog.oidcInProgress
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: passwordField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                        implicitHeight: Theme.touchTarget
                    }
                    padding: Theme.sp.s3

                    Keys.onReturnPressed: loginButton.clicked()
                }
            }

            // Error message
            Text {
                text: dialog.errorMessage
                font.pixelSize: Theme.fontSize.sm
                color: Theme.danger
                visible: dialog.errorMessage !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }

            // "We couldn't open your browser — here is the link."
            //
            // THE surface for a sign-in that cannot hand off to a browser.
            // Before it existed, the Linux release tarball's failure to
            // launch xdg-open produced a click that did nothing, said
            // nothing and logged nothing, for five minutes, and then timed
            // out. Warning-coloured rather than danger-coloured on purpose:
            // nothing has failed yet, there is just one step the app cannot
            // take for the user.
            //
            // The one block in this file with no `mode` in its visibility,
            // and deliberately: a browser that will not open is a property
            // of the machine, not of the screen. It can strike the ID
            // button on "choose", the OIDC button on "address", and a
            // per-server sign-in running behind "signedIn" — and the
            // answer is the same link in all three. It sits with the error
            // text, below whichever screen is up, so it cannot be scrolled
            // off by a screen that does not know about it.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s2
                visible: dialog.manualAuthUrl !== ""

                Text {
                    Layout.fillWidth: true
                    text: "Couldn't open a browser on this computer. Open this "
                          + "link yourself to finish signing in — this window "
                          + "keeps waiting for it."
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.warn
                    wrapMode: Text.Wrap
                }

                // Selectable, and wrapping, because the whole point is that
                // the user gets these ~400 characters out of the app intact.
                // Read-only TextEdit rather than Text: Text cannot be
                // selected with the mouse, and a link nobody can select is
                // a link nobody can use.
                TextArea {
                    id: manualUrlField
                    Layout.fillWidth: true
                    text: dialog.manualAuthUrl
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    color: Theme.fg1
                    leftPadding: Theme.sp.s3
                    rightPadding: Theme.sp.s3
                    topPadding: Theme.sp.s2
                    bottomPadding: Theme.sp.s2
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: Theme.line
                        border.width: 1
                    }
                }

                Button {
                    id: copyLinkButton
                    Layout.alignment: Qt.AlignRight
                    contentItem: Text {
                        text: "Copy link"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: copyLinkButton.hovered ? Theme.accentDim : Theme.accent
                        radius: Theme.r2
                        implicitHeight: 32
                        implicitWidth: 110
                    }
                    onClicked: serverManager.copyToClipboard(dialog.manualAuthUrl)
                }
            }

            // Connecting indicator
            Text {
                text: dialog.oidcInProgress ? "Waiting for browser login..." : "Connecting..."
                font.pixelSize: Theme.fontSize.md
                color: Theme.fg2
                visible: dialog.isConnecting || dialog.oidcInProgress
                Layout.alignment: Qt.AlignHCenter
            }

            // Password auth buttons
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                // `probed` is load-bearing: no Register button exists until the
                // server has said, in a login-flows document of its own, that
                // it accepts m.login.password. Assuming it did is the entire
                // bug this dialog is being fixed for.
                visible: dialog.mode === "address" && dialog.probed && dialog.passwordAvailable
                         && !dialog.checkingFlows
                         && (!dialog.oidcAvailable || dialog.showPasswordFallback)

                // Ghost Register — secondary action, soft border, fg1.
                Button {
                    id: registerBtn
                    Layout.fillWidth: true
                    enabled: !dialog.isConnecting && !dialog.oidcInProgress
                    contentItem: Text {
                        text: "Register"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.medium
                        color: registerBtn.enabled ? Theme.fg1 : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: registerBtn.hovered && registerBtn.enabled ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitHeight: Theme.touchTarget
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        dialog.errorMessage = "";
                        if (urlField.text.trim() === "" || usernameField.text.trim() === "" || passwordField.text.trim() === "") {
                            dialog.errorMessage = "All fields are required";
                            return;
                        }
                        dialog.isConnecting = true;
                        dialog.awaitingSingleJoin = true;
                        serverManager.registerServer(dialog.targetUrl(), usernameField.text.trim(), passwordField.text.trim());
                    }
                }

                // Primary Login — accent filled.
                Button {
                    id: loginButton
                    Layout.fillWidth: true
                    enabled: !dialog.isConnecting && !dialog.oidcInProgress
                    contentItem: Text {
                        text: "Login"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: loginButton.enabled ? Theme.onAccent : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: !loginButton.enabled ? Theme.bg2
                             : (loginButton.hovered ? Theme.accentDim : Theme.accent)
                        radius: Theme.r2
                        implicitHeight: Theme.touchTarget
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        dialog.errorMessage = "";
                        if (urlField.text.trim() === "" || usernameField.text.trim() === "" || passwordField.text.trim() === "") {
                            dialog.errorMessage = "All fields are required";
                            return;
                        }
                        dialog.isConnecting = true;
                        dialog.awaitingSingleJoin = true;
                        serverManager.addServer(dialog.targetUrl(), usernameField.text.trim(), passwordField.text.trim());
                    }
                }
            }

            // Cancel — ghost text link, no bg at all. Not offered on the
            // confirmation screens: there "Continue" is the dismiss, and a
            // second way out next to "Not you?" only muddies which one
            // keeps the account.
            Button {
                id: cancelBtn
                visible: dialog.mode === "choose" || dialog.mode === "address"
                Layout.fillWidth: true
                contentItem: Text {
                    text: "Cancel"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: cancelBtn.hovered ? Theme.fg1 : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { color: "transparent"; implicitHeight: Theme.touchTarget }
                onClicked: dialog.close()
            }
        }
    }
}

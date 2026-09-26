# Accessibility in the BSFChat QML client

This document is the convention. It was written before the annotation pass, so
that ~330 controls across 53 files would be described the same way rather than
53 different ways, and so the regression guard in
`tests/test_qml_hygiene.cpp` has something concrete to enforce.

## Why this exists at all

Qt bridges `QAccessible` to the platform screen reader: VoiceOver on macOS and
iOS, TalkBack on Android, Narrator/NVDA on Windows via UI Automation. The
bridge is automatic, but it only reports what the QML declares. A `Rectangle`
with a `TapHandler` and an `Icon` inside it is, to a screen reader, an unnamed
nothing. Before this pass the client had 31 `Accessible.` usages in 9 of 53
files, so almost every control in the app was exactly that.

None of this is a checkbox exercise. A screen-reader user driving BSFChat has
to be able to find a channel, know who is in it, know whether their microphone
is live, and hear an incoming message. Those four things are the bar.

## Vocabulary

### 1. Strings are always `qsTr()`

**Decision: every user-facing accessible string goes through `qsTr()`.**

Before this pass the codebase was inconsistent — `BotBadge.qml` used `qsTr()`,
the other eight annotated files used bare literals. Resolved in favour of
`qsTr()` everywhere, for one reason: `Accessible.name` and
`Accessible.description` are *read aloud to the user*. They are user-facing
text in exactly the way a `Text { text: }` is, and the rule for visible strings
should not change just because the string is spoken instead of drawn.

The client has no translation catalogue today — there is no `.ts` file and no
`lupdate` step. `qsTr()` still costs nothing at runtime (it is a lookup in an
empty catalogue that returns the source string) and it means the day someone
does add translations, the accessible layer is already in the extraction set
rather than being a second, invisible migration. Annotating without it would
have meant doing the work twice.

Interpolated strings use `.arg()`, not `+`, so a translator can reorder:

```qml
Accessible.name: qsTr("%1 said %2").arg(sender).arg(body)   // yes
Accessible.name: qsTr("said by ") + sender                  // no
```

### 2. Roles

Use the narrowest role that is true. The mapping used throughout:

| What it is | Role |
| --- | --- |
| Anything you press that does something once | `Accessible.Button` |
| A row in a list you select (channel, member, search hit, emoji) | `Accessible.ListItem` |
| The list itself, when it needs its own name | `Accessible.List` |
| Text you read but cannot edit or press | `Accessible.StaticText` |
| A single-line text entry | `Accessible.EditableText` |
| The message composer (multiline) | `Accessible.EditableText` |
| A two-state control (mute, deafen, camera, a settings toggle) | `Accessible.CheckBox` |
| One of a mutually exclusive set (presence, allow/neutral/deny) | `Accessible.RadioButton` |
| An item in a menu | `Accessible.MenuItem` |
| A tab in a tab strip | `Accessible.PageTab` |
| A picture or a video surface | `Accessible.Graphic` |
| A slider (volume, gain) | `Accessible.Slider` |
| A dropdown | `Accessible.ComboBox` |
| A modal dialog's root | `Accessible.Dialog` |
| A non-modal popup / drawer / sheet root | `Accessible.Dialog` |
| A grouping container that needs a name (the timeline, the member pane) | `Accessible.Pane` |
| A whole message in the timeline | `Accessible.StaticText` |
| A status strip (connection banner, toast) | `Accessible.AlertMessage` |
| Anything decorative | *no role* — see §6 |

`Accessible.Button` on a `TapHandler`/`MouseArea` construct goes on the visual
item that owns the handler, not on the handler — attached accessibility
properties only take effect on an `Item`.

### 3. Name vs description

- **`Accessible.name` is the label.** Short, specific, no punctuation at the
  end, capitalised as a label ("Leave voice channel", not "leave voice
  channel." and not "Button to leave the voice channel"). It answers *what is
  this*. It must be unique enough to be useful when read out of context,
  because a screen reader's rotor lists names with nothing around them: four
  buttons all named "Remove" are four identical rows.
- **`Accessible.description` is the consequence.** It answers *what happens if
  I activate it*, or gives the extra context that would clutter the name.
  Optional. Leave it out rather than restating the name.

```qml
Accessible.name: qsTr("Leave voice channel")
Accessible.description: qsTr("Disconnect from the call")
```

Never put the control type in the name. The role already says "button"; a name
of "Mute button" is read as "Mute button, button".

### 4. Names are derived from what is on screen

If the control has a visible text label, the accessible name is that label. If
it is icon-only, the accessible name is the label it *would* have had — the
tooltip text is usually already the right string, and where a `ToolTip` exists
the name matches it.

If the control is about a specific thing, the name names the thing, not the
verb alone: `qsTr("Remove %1").arg(memberName)`, not `qsTr("Remove")`.

### 5. State is part of the announcement

This is the part that is easy to get wrong and that matters most in the voice
surfaces. A mute button that says "Mute" whether or not you are muted is worse
than no label, because it implies a state it is not reporting.

Two-state controls declare:

```qml
Accessible.role: Accessible.CheckBox
Accessible.checkable: true
Accessible.checked: voice.micMuted
Accessible.name: qsTr("Mute")
Accessible.description: voice.micMuted
    ? qsTr("Your microphone is off. Activate to turn it on.")
    : qsTr("Your microphone is live. Activate to mute it.")
Accessible.onToggleAction: micBtn.clicked()
Accessible.onPressAction: micBtn.clicked()
```

The platform reads `checked` itself ("Mute, checked"), so the name stays the
stable noun and does not flip the way the tooltip does. Name the *suppressing*
state, in the app's own vocabulary — "Mute" checked, "Deafen" checked — so that
"checked" is unambiguous; "Microphone, checked" leaves the listener guessing
which way round it is. The description then says it in plain words anyway,
because that guess is not one to leave to a listener mid-call.

For state that is not binary — a member who is speaking, a channel with
unread messages — fold it into the name, because there is no attribute for it:

```qml
Accessible.name: unreadCount > 0
    ? qsTr("%1, %n unread message(s)", "", unreadCount).arg(channelName)
    : channelName
```

Bindings do the announcing: `Accessible.name` is a QML binding like any
other, so when the underlying state changes the accessible name changes, and
Qt emits the corresponding `QAccessible::NameChanged` / `StateChanged` event.
Do not write imperative updates.

### 6. Decorative elements are explicitly ignored, never merely unnamed

An unnamed element is a bug the guard will report. An element that genuinely
carries no information says so:

```qml
Accessible.ignored: true
```

This is set at the root of `Icon.qml`, so every icon in the app is excluded in
one place — an icon is always either inside a named control (whose name already
covers it) or purely ornamental. Same for `ThemedScrollBar.qml` (the scroll
position is exposed by the flickable, not the bar), pulse dots, dividers,
gradient scrims and hover overlays.

`Accessible.ignored: true` on a parent does **not** hide its children; it
removes that one node from the tree. To collapse a composite into a single
announcement, name the parent and ignore the children — see §7.

### 7. One announcement per thing: the message timeline

A message bubble is built from a dozen `Text` elements: sender, timestamp,
edited marker, body split into styled spans by the markdown renderer, reaction
chips, a reply quote. Left alone, a screen reader walks all of them, and
reading one message takes twelve swipes and sounds like a stack trace.

The rule: **the bubble is the accessible node, its text children are not.**

```qml
// MessageBubble.qml root
Accessible.role: Accessible.StaticText
Accessible.name: <sender>, <body>, <time>, assembled in one string
Accessible.ignored: false
```

and each interior `Text` that is part of that sentence gets
`Accessible.ignored: true`. Interactive children of the bubble (the reaction
buttons, the hover action row) are *not* ignored — they remain separately
reachable, because they are separately actionable.

The assembled string is built by a helper so the ordering is in one place, and
reads as a sentence: sender, then body, then time, then any state suffix
(edited, pending, failed). Time last because it is the least important part
and a screen-reader user should not have to sit through a timestamp before
hearing the message.

### 8. Transient surfaces announce themselves

A connection banner appearing is precisely the thing a screen-reader user
cannot see happen. Qt 6.8 added `Accessible.announce(message, politeness)`
(`QQuickAccessibleAttached::announce`); the client's minimum is Qt 6.8 on
Android and 6.10 elsewhere, so it is available everywhere we ship.

Transient surfaces (connection banner, toasts, inline errors) therefore both

1. carry `Accessible.role: Accessible.AlertMessage` and a name, so the surface
   is reachable and readable after the fact, and
2. call `Accessible.announce(...)` when they become visible, so the user hears
   it at the time.

```qml
onVisibleChanged: if (visible)
    root.Accessible.announce(root.Accessible.name,
                             Accessible.AnnouncementPoliteness.Assertive)
```

Politeness: `Assertive` for anything the user must act on (disconnected,
session expired, an error), `Polite` for anything informational (reconnected,
message sent, an incoming message in the timeline).

`AnnouncementPoliteness` is a *scoped* enum (`isScoped: true` in QtQuick's
`plugins.qmltypes`), so it is spelled out in full —
`Accessible.AnnouncementPoliteness.Assertive`, not `Accessible.Assertive`.
The short form may also resolve; the long form definitely does, and none of
this can be checked without running the app.

`announce()` is best-effort — some platform bridges drop it. It is an addition
to a correctly named node, never a substitute for one.

### 9. Focus order

Qt derives keyboard focus order from the item tree, which is usually right
because it matches the visual order. Where it does not, or where a surface
opens and focus has nowhere sensible to be:

- Every dialog sets initial focus explicitly on open, on the control the user
  is most likely to want — the first text field, or the primary action if
  there is no field.
- **Except a destructive confirmation**, which focuses the way out, not the
  way through. "Delete this message?" opening with focus on Delete means a
  space-bar press destroys the message. Focus Cancel.
- `onAboutToShow` is too early: it fires from `prepareEnterTransition()`,
  before the content item is visible, and a `forceActiveFocus()` there does
  not stick. Use `onOpened`.
- Nothing sets `activeFocusOnTab: false` on something a keyboard user needs.
- Drawers on mobile move focus into themselves on open and back to the opener
  on close.
- Purely decorative items are never in the tab chain.

### 10. `objectName`

The codebase had zero `objectName` before this pass and still has none as a
requirement. It is worth knowing why the two are not the same thing:
`objectName` is a *test and tooling* handle, and on some platforms it leaks
into the accessibility tree as a fallback name when nothing better is
available. Relying on that fallback is how you end up with a screen reader
saying "rect_14". Names come from `Accessible.name`. If a future UI test needs
handles, add `objectName` for that purpose, separately, and do not let it
become the accessible name.

## 11. Popups, dialogs and drawers: the attachment warning is a Qt wart

`Accessible.role` and `Accessible.name` go on the `Popup` / `Dialog` / `Drawer`
itself, and Qt will log

    Accessible must be attached to an Item

every time one is created. That warning is wrong about this case, and the
placement is right. `QQuickPopup` does derive from `QObject` rather than
`QQuickItem`, which is what the warning is reacting to — but Qt Quick Controls
reads the attached properties off the popup anyway and forwards them to the
popup *item*, which is the node a screen reader actually meets:

- `QQuickPopupItem::accessibleRole()` returns
  `QQuickPopup::effectiveAccessibleRole()`, which reads the role from the
  popup's attached object;
- `QQuickPopupItem::accessibilityActiveChanged()` contains a branch whose own
  comment reads "The user set Accessible.name on the Popup", and copies that
  name onto the popup item.

(Both in `qtdeclarative/src/quicktemplates/qquickpopupitem.cpp`.)

Putting the name on the popup's `contentItem` instead leaves the dialog node
itself unnamed — a screen reader announces an anonymous dialog — and adds a
second, redundant node inside it. So: on the popup, warning and all. This was
tried the other way round during the annotation pass and reverted.

A `Window` is a different matter: attached accessibility genuinely does nothing
there, and `QAccessibleQuickWindow` reads the window's `title` instead. Name
a window through `title:`, and put roles and names on its content items.

## What the guard enforces

`tests/test_qml_hygiene.cpp :: everyInteractiveControlIsNamedOrDeliberatelyIgnored`
scans the QML source text for interactive constructs —
`Button`, `ToolButton`, `RoundButton`, `TabButton`, `ItemDelegate`,
`MenuItem`, `SwipeDelegate`, `CheckBox`, `RadioButton`, `Switch`, `Slider`,
`ComboBox`, `TapHandler`, and `MouseArea` blocks that carry an `onClicked` or
`onTapped` — and requires each one to be inside a scope that declares either
`Accessible.role` **and** `Accessible.name`, or `Accessible.ignored: true`.

It is a text scan, with the limits that implies (see the guard's own comment).
It cannot tell you the name is *good*, only that one exists. It exists to stop
the next 200 controls from being added unnamed, which is what happened to the
first 300.

## What is deliberately not covered

- **Colour contrast** is guarded separately and already, by
  `senderColoursAreTellableApartAndLegible` in the same file.
- **Dynamic type / text scaling** is not addressed by this pass at all. It is
  real work and it is separate work.
- **Keyboard-only operation on desktop** is partly a function of focus order
  (§9) and partly of whether every action has a non-pointer route. The second
  half is not audited here.
- **Reduced motion.** Not addressed.

## Manual verification is still required

Nothing in this document, and nothing in the guard, has been checked against a
real screen reader. A source scan cannot tell you that an announcement is
comprehensible, that focus lands somewhere sensible, or that the timeline does
not read a message twice. See the "needs manual screen-reader testing" section
of the pull request that introduced this file for the specific flows a human
should walk on iOS with VoiceOver and Android with TalkBack.

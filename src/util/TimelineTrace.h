#pragma once

#include <QLoggingCategory>

// Per-channel-switch timing: "opening a channel is slow" is not a measurable
// claim until something says WHERE the time went.
//
// One line per stage of a channel open, all stamped with the milliseconds
// since setActiveRoom() was called, so a log read top-to-bottom reconstructs
// the whole switch:
//
//   open room=… "general" cachedRows=0
//   /messages page=1 from="" limit=50 rt=214ms events=50 rows=+7 more=yes
//   /messages page=2 from="s8412" limit=100 rt=198ms events=100 rows=+11 more=yes
//   /messages page=3 from="s8312" limit=100 rt=205ms events=64 rows=+12 more=no
//   fill stop=StartOfRoom pages=3 rows=30 net=617ms insert=9ms sinceOpen=631ms
//   /members rt=190ms n=9 sinceOpen=193ms
//   visible rows=30 sinceOpen=648ms
//
// The three numbers that answer the question are `rt` (the server and the
// network), `insert` (the model's own work) and the gap between the last fill
// line and `visible` (QML layout). `pages` says whether one switch costs one
// round trip or ten: an open fill keeps paginating until it has
// kHistoryOpenTargetRows RENDERABLE rows, and a window full of edits, voice
// membership churn or join/leave state renders almost nothing (see
// util/HistoryFill.h), so a channel can need several serial requests before
// the timeline shows anything at all.
//
// QtInfoMsg by default, unlike the other bsfchat.* categories, for the same
// reason bsfchat.mobile.keyboard is: the case that needs it most is a phone,
// where the log is pulled off the device after the fact and nobody can go and
// set QT_LOGGING_RULES first. It fires a handful of lines per channel switch,
// not per network poll, so the cost of leaving it on is a rounding error
// against /sync's own traffic.
Q_DECLARE_LOGGING_CATEGORY(logTimeline)

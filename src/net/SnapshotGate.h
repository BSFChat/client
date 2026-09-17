#pragma once

// Publish gate for the QML-facing snapshot lists (U-H2).
//
// ServerConnection hands QML whole QVariantLists — categorized rooms, DMs,
// server members, bans. A Repeater/ListView fed one of those destroys and
// rebuilds every delegate when its model is reassigned, so the change signal
// must only fire when the list really differs from the one QML already holds.
// Every such snapshot is published through here: build the fresh value, and
// emit only if this returns true.
//
// Header-only and free of ServerConnection so tests/test_snapshot_gate.cpp can
// pin the comparison semantics the gate depends on without a connection, an
// event loop or a network.

#include <utility>

namespace bsfchat::client {

// Replaces `published` with `fresh` and returns true when they differ;
// leaves `published` untouched and returns false when they are equal.
//
// For QVariantList this is a deep value comparison as long as every leaf is a
// string/number/bool/list/map — which is all these snapshots contain. Order is
// significant: a reordered list is a changed list, which is what makes the DM
// section re-sort when a message lands in a conversation further down.
template <typename T>
bool publishIfChanged(T& published, T fresh)
{
    if (published == fresh) return false;
    published = std::move(fresh);
    return true;
}

} // namespace bsfchat::client

// Ed25519 public key(s) that sign desktop release manifests.
//
// The release CI job writes SHA256SUMS (one line per published asset plus a
// `# release: <tag>` header) and signs it with the private half of a key
// listed here, held in the GitHub secret RELEASE_SIGNING_KEY. The Updater
// refuses to install anything whose SHA256SUMS does not verify against one
// of these keys. See docs/release-signing.md for key generation, the secret
// and rotation.
//
// Format: the raw 32-byte Ed25519 public key, hex. Get it from a PEM with
//   openssl pkey -in key.pem -pubout -outform DER | tail -c 32 | od -An -tx1 -v | tr -d ' \n'
//
// ROTATION: add the new key, ship a release signed with the OLD key that
// carries both, then switch the secret to the new key. Remove the old key
// only once no supported build would still need it. A build that knows no
// key matching the current secret can never auto-update again.
#pragma once

#include <array>

namespace bsfchat::updates {

inline constexpr std::array<const char*, 1> kReleaseSigningKeysHex = {
    // 2026-11-15, primary.
    // -----BEGIN PUBLIC KEY-----
    // MCowBQYDK2VwAyEARV05N02P2wpj6FES0xQYmN61nEwpef4WAakNFAY3Ux8=
    // -----END PUBLIC KEY-----
    "455d39374d8fdb0a63e85112d3141898deb59c4c2979fe1601a90d140637531f",
};

} // namespace bsfchat::updates

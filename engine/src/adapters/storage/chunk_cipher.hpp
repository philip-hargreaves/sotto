#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace sotto::store {

// AES-256-GCM for audio chunks under a per-session key (ADR-0017). The nonce
// is the chunk sequence number, unique for the key's lifetime; the session id
// and sequence are authenticated as associated data, so a chunk cannot be
// moved between sessions or renumbered undetected. Destroying the key is the
// cancel guarantee: without it, every copy of the ciphertext is noise.
class ChunkCipher {
   public:
    static ChunkCipher Generate();
    static ChunkCipher FromWrapped(std::span<const std::uint8_t> wrapped);

    // The key, DPAPI-protected for the current user; safe to persist
    std::vector<std::uint8_t> Wrapped() const;

    // Returns ciphertext followed by the 16-byte tag
    std::vector<std::uint8_t> Seal(std::string_view session_id, std::uint64_t seq,
                                   std::span<const std::uint8_t> plain) const;

    // Throws if the payload fails authentication for any reason
    std::vector<std::uint8_t> Open(std::string_view session_id, std::uint64_t seq,
                                   std::span<const std::uint8_t> sealed) const;

    ChunkCipher(ChunkCipher&&) noexcept;
    ChunkCipher& operator=(ChunkCipher&&) noexcept;
    ~ChunkCipher();

   private:
    struct Impl;
    explicit ChunkCipher(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace sotto::store

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace sotto::store {

// Keeps each stream's IVs disjoint; every domain counts seq from zero
enum class Domain : std::uint8_t {
    kAudio = 0,
    kTurns = 1,
    kNote = 2,
    kPatient = 3,
    kTranslation = 4,
    kLabel = 5,
};

// AES-256-GCM under a per-session key. The IV is domain plus sequence,
// never reused; domain, session id and sequence are authenticated, so a
// payload cannot be moved or renumbered undetected. Destroying the key
// makes every copy of the ciphertext unreadable, which is what cancel means.
class ChunkCipher {
   public:
    static ChunkCipher Generate();
    static ChunkCipher FromWrapped(std::span<const std::uint8_t> wrapped);

    // The key, DPAPI-protected for the current user; safe to persist
    std::vector<std::uint8_t> Wrapped() const;

    // Returns ciphertext followed by the 16-byte tag
    std::vector<std::uint8_t> Seal(Domain domain, std::string_view session_id, std::uint64_t seq,
                                   std::span<const std::uint8_t> plain) const;

    // Throws if the payload fails authentication for any reason
    std::vector<std::uint8_t> Open(Domain domain, std::string_view session_id, std::uint64_t seq,
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

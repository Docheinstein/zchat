#include "cipher.hpp"

#include <cstdint>
#include <random>

namespace zchat::cipher {

namespace {

    constexpr std::string_view magic = "ZX1";
    constexpr std::size_t nonce_bytes = 4;

    // FNV-1a, to turn the passphrase into the key at compile time.
    constexpr std::uint64_t hash(std::string_view s) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (char c : s) {
            h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
        }
        return h;
    }

    constexpr std::uint64_t key = hash("zchat: rawr means I love you in dinosaur");

    class Keystream {
    public:
        explicit Keystream(std::uint32_t nonce) :
            state_(key ^ (static_cast<std::uint64_t>(nonce) * 0x9e3779b97f4a7c15ull)) {}

        unsigned char next() {
            if (used_ == 8) {
                // splitmix64
                std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
                z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
                z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
                block_ = z ^ (z >> 31);
                used_ = 0;
            }
            return static_cast<unsigned char>(block_ >> (8 * used_++));
        }

    private:
        std::uint64_t state_;
        std::uint64_t block_ = 0;
        int used_ = 8;
    };

    std::uint32_t random_nonce() {
        thread_local std::mt19937 rng(std::random_device {}());
        return static_cast<std::uint32_t>(rng());
    }

} // namespace

std::string scramble(std::string_view plain) {
    const std::uint32_t nonce = random_nonce();
    std::string out(magic);
    for (std::size_t i = 0; i < nonce_bytes; ++i) {
        out += static_cast<char>(nonce >> (8 * i));
    }
    Keystream ks(nonce);
    unsigned char previous = static_cast<unsigned char>(nonce);
    for (char c : plain) {
        previous = static_cast<unsigned char>((static_cast<unsigned char>(c) ^ ks.next()) + previous);
        out += static_cast<char>(previous);
    }
    return out;
}

std::optional<std::string> unscramble(std::string_view data) {
    if (!data.starts_with(magic) || data.size() < magic.size() + nonce_bytes) {
        return std::nullopt;
    }
    data.remove_prefix(magic.size());
    std::uint32_t nonce = 0;
    for (std::size_t i = 0; i < nonce_bytes; ++i) {
        nonce |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << (8 * i);
    }
    data.remove_prefix(nonce_bytes);
    Keystream ks(nonce);
    unsigned char previous = static_cast<unsigned char>(nonce);
    std::string out;
    out.reserve(data.size());
    for (char c : data) {
        const auto scrambled = static_cast<unsigned char>(c);
        out += static_cast<char>(static_cast<unsigned char>(scrambled - previous) ^ ks.next());
        previous = scrambled;
    }
    return out;
}

} // namespace zchat::cipher

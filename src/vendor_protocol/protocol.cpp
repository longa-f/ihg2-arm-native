#include "ihg2_arm/vendor_protocol/protocol.hpp"
#include <algorithm>
#include <cctype>
#include <memory>
#ifdef IHG2_USE_MBEDTLS
#include <mbedtls/aes.h>
#else
#include <openssl/evp.h>
#endif
namespace ihg2::arm {
namespace {
constexpr std::array<std::uint8_t, 8> magic{0x5a, 0xa5, 0xaa, 0x55, 0x5a, 0xa5, 0xaa, 0x55};
constexpr std::array<std::uint8_t, 4> inner_magic{0xa5, 0xa5, 0x5a, 0x5a};
constexpr std::array<std::uint8_t, 16> iv{0x56, 0x2e, 0x17, 0x99, 0x6d, 0x09, 0x3d, 0x28,
                                          0xdd, 0xb3, 0xba, 0x69, 0x5a, 0x2e, 0x6f, 0x58};
std::uint16_t u16(std::span<const std::uint8_t> p, std::size_t at) {
    return static_cast<std::uint16_t>(p[at] | (static_cast<unsigned>(p[at + 1]) << 8));
}
void put16(Bytes &p, std::size_t at, std::uint16_t v) {
    p[at] = static_cast<std::uint8_t>(v);
    p[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
Bytes aes(std::span<const std::uint8_t> data, const Key &key, bool encrypt) {
    if (data.empty() || data.size() % 16 || data.size() > 8192)
        throw ProtocolError("invalid AES length");
#ifdef IHG2_USE_MBEDTLS
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    const int setup = encrypt ? mbedtls_aes_setkey_enc(&ctx, key.data(), 128)
                              : mbedtls_aes_setkey_dec(&ctx, key.data(), 128);
    if (setup != 0) {
        mbedtls_aes_free(&ctx);
        throw ProtocolError("AES initialization failed");
    }
    Bytes out(data.size());
    auto chain = iv;
    const int result = mbedtls_aes_crypt_cbc(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                                              data.size(), chain.data(), data.data(), out.data());
    mbedtls_aes_free(&ctx);
    if (result != 0) throw ProtocolError("AES operation failed");
    return out;
#else
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    if (!ctx ||
        EVP_CipherInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key.data(), iv.data(), encrypt ? 1 : 0) !=
            1 ||
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1)
        throw ProtocolError("AES initialization failed");
    Bytes out(data.size() + 16);
    int written = 0, tail = 0;
    if (EVP_CipherUpdate(ctx.get(), out.data(), &written, data.data(), static_cast<int>(data.size())) != 1 ||
        EVP_CipherFinal_ex(ctx.get(), out.data() + written, &tail) != 1)
        throw ProtocolError("AES operation failed");
    out.resize(static_cast<std::size_t>(written + tail));
    return out;
#endif
}
template <std::size_t N> std::array<std::uint8_t, N> hex(std::string text) {
    if (text.starts_with("0x"))
        text.erase(0, 2);
    if (text.size() != N * 2)
        throw ProtocolError("invalid hexadecimal length");
    std::array<std::uint8_t, N> out{};
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        throw ProtocolError("invalid hexadecimal digit");
    };
    for (std::size_t i = 0; i < N; ++i)
        out[i] = static_cast<std::uint8_t>(digit(text[2 * i]) * 16 + digit(text[2 * i + 1]));
    return out;
}
} // namespace
Key parse_key(const std::string &text) {
    return hex<16>(text);
}
Mac parse_mac(const std::string &text) {
    std::string compact;
    for (char c : text)
        if (c != ':' && c != '-')
            compact += c;
    return hex<6>(compact);
}
std::uint16_t checksum(std::span<const std::uint8_t> data) {
    std::uint32_t sum = 0xbeaf;
    for (auto b : data)
        sum += b;
    return static_cast<std::uint16_t>(sum);
}
Json bounded_json(std::string_view text, std::size_t max_bytes, std::size_t max_nodes) {
    if (text.size() > max_bytes)
        throw ProtocolError("JSON size limit");
    // Scan nesting before the recursive JSON DOM parser. Ignore brackets inside strings.
    std::size_t depth = 0;
    bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32)
                throw ProtocolError("JSON depth limit");
        } else if (c == '}' || c == ']') {
            if (depth == 0)
                throw ProtocolError("invalid JSON nesting");
            --depth;
        }
    }
    try {
        auto result = Json::parse(text);
        std::vector<const Json *> work{&result};
        std::size_t nodes = 0;
        while (!work.empty()) {
            auto value = work.back();
            work.pop_back();
            if (++nodes > max_nodes)
                throw ProtocolError("JSON node limit");
            if (value->is_object())
                for (auto it = value->begin(); it != value->end(); ++it) {
                    if (it.key().size() > 128)
                        throw ProtocolError("JSON key limit");
                    work.push_back(&it.value());
                }
            else if (value->is_array())
                for (const auto &item : *value)
                    work.push_back(&item);
            else if (value->is_string() && value->get_ref<const std::string &>().size() > 4096)
                throw ProtocolError("JSON string limit");
        }
        return result;
    } catch (const nlohmann::json::exception &) {
        throw ProtocolError("invalid JSON");
    }
}
Bytes encode_packet(const Json &payload, const Key &key, const Identity &id, std::uint16_t cmd,
                    std::uint16_t count, std::uint16_t type, std::uint16_t flags) {
    const std::string body = payload.is_null() ? "" : payload.dump();
    if (body.size() > 8000)
        throw ProtocolError("payload size limit");
    Bytes plain((12 + body.size() + 15) / 16 * 16, 0);
    std::copy(inner_magic.begin(), inner_magic.end(), plain.begin());
    put16(plain, 6, type);
    put16(plain, 8, static_cast<std::uint16_t>(body.size()));
    put16(plain, 10, flags);
    std::copy(body.begin(), body.end(), plain.begin() + 12);
    put16(plain, 4, checksum(std::span(plain).first(12 + body.size())));
    auto ciphertext = aes(plain, key, true);
    Bytes packet(0x38 + ciphertext.size(), 0);
    std::copy(magic.begin(), magic.end(), packet.begin());
    put16(packet, 0x24, id.devtype);
    put16(packet, 0x26, cmd);
    put16(packet, 0x28, count);
    std::reverse_copy(id.mac.begin(), id.mac.end(), packet.begin() + 0x2a);
    for (unsigned i = 0; i < 4; ++i)
        packet[0x30 + i] = static_cast<std::uint8_t>(id.device_id >> (8 * i));
    put16(packet, 0x34, checksum(plain));
    std::copy(ciphertext.begin(), ciphertext.end(), packet.begin() + 0x38);
    put16(packet, 0x20, checksum(packet));
    return packet;
}
Frame decode_packet(std::span<const std::uint8_t> packet, const Key &key, const Identity &id, bool strict,
                    bool check_payload) {
    if (packet.size() < 0x48 || packet.size() > 8192 || (packet.size() - 0x38) % 16 ||
        !std::equal(magic.begin(), magic.end(), packet.begin()))
        throw ProtocolError("invalid packet structure");
    Bytes outer(packet.begin(), packet.end());
    const auto expected = u16(outer, 0x20);
    put16(outer, 0x20, 0);
    if (checksum(outer) != expected)
        throw ProtocolError("outer checksum mismatch");
    if (strict) {
        if (!std::equal(id.mac.rbegin(), id.mac.rend(), packet.begin() + 0x2a))
            throw ProtocolError("MAC mismatch");
        std::uint32_t device = 0;
        for (unsigned i = 0; i < 4; ++i)
            device |= static_cast<std::uint32_t>(packet[0x30 + i]) << (8 * i);
        if (device != id.device_id || u16(packet, 0x24) != id.devtype)
            throw ProtocolError("device identity mismatch");
    }
    auto plain = aes(packet.subspan(0x38), key, false);
    if (plain.size() < 12 || !std::equal(inner_magic.begin(), inner_magic.end(), plain.begin()))
        throw ProtocolError("inner magic mismatch");
    const std::size_t end = 12 + u16(plain, 8);
    if (end > plain.size())
        throw ProtocolError("inner length mismatch");
    auto inner = Bytes(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(end));
    auto inner_expected = u16(inner, 4);
    put16(inner, 4, 0);
    if (checksum(inner) != inner_expected)
        throw ProtocolError("inner checksum mismatch");
    if (check_payload && checksum(plain) != u16(packet, 0x34))
        throw ProtocolError("payload checksum mismatch");
    Frame frame{u16(packet, 0x26), u16(packet, 0x28), u16(packet, 0x22),
                u16(plain, 6),     u16(plain, 10),    nullptr};
    // IHG2's normal data responses use 0x0b03. The gateway's heartbeat ACK
    // (0x03ed) uses an empty inner frame with type 0x0000 on the installed
    // firmware; accepting only 0x0b03 makes the bridge discard every heartbeat.
    const bool heartbeat_ack = frame.command == response_heartbeat && frame.message_type == 0x0000;
    if (frame.message_type != 0x0b03 && !heartbeat_ack)
        throw ProtocolError("unexpected inner message type");
    // Binary-prefix reports remain valid; unknown binary bodies never become device state.
    auto start = std::find_if(plain.begin() + 12, plain.begin() + static_cast<std::ptrdiff_t>(end),
                              [](auto b) { return b == '{' || b == '['; });
    if (start != plain.begin() + static_cast<std::ptrdiff_t>(end)) {
        auto finish = plain.begin() + static_cast<std::ptrdiff_t>(end);
        while (finish != start && finish[-1] == 0)
            --finish;
        frame.payload = bounded_json(std::string_view(reinterpret_cast<const char *>(&*start),
                                                      static_cast<std::size_t>(finish - start)));
        if (!frame.payload.is_object())
            throw ProtocolError("device payload must be object");
    }
    return frame;
}
std::string fingerprint(std::span<const std::uint8_t> data) {
    std::array<unsigned char, 32> digest{};
    unsigned size = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1)
        throw ProtocolError("digest failed");
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned i = 0; i < size; ++i) {
        out += digits[digest[i] >> 4];
        out += digits[digest[i] & 15];
    }
    return out;
}
} // namespace ihg2::arm

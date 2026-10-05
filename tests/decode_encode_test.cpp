#include "catch2/generators/catch_generators_range.hpp"

#include <sparq/decoder.hpp>
#include <sparq/encoder.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace spq;

namespace
{
    using Bytes = std::vector<std::uint8_t>;

    // Buffer-owning decoder, sized for the largest frame used in these tests (506 bytes).
    using test_decoder = decoder<4096u, 1024u>;

    std::array<std::uint8_t, 1024u> test_buffer{};

    constexpr std::span<std::uint8_t> fresh_buffer()
    {
        test_buffer.fill(0xCC);
        return test_buffer;
    }

    [[nodiscard]]
    constexpr Bytes to_bytes(encoded_message const& message)
    {
        return {message.data.begin(), message.data.end()};
    }

    [[nodiscard]]
    constexpr bool same_bits(float const a, float const b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    // sample::value is a double, so compare the float that went in with the double that came out.
    // NaNs only have to stay NaNs, every other value has to survive bit-exact (incl. -0.0 and denormals).
    [[nodiscard]]
    bool same_value(float const expected, double const actual)
    {
        if (std::isnan(expected))
        {
            return std::isnan(actual);
        }

        return same_bits(expected, static_cast<float>(actual));
    }

    [[nodiscard]]
    constexpr Bytes make_ids(std::size_t const count)
    {
        Bytes ids{};
        for (std::size_t i = 0; i < count; ++i)
        {
            ids.push_back(static_cast<std::uint8_t>(i * 3u));
        }
        return ids;
    }

    [[nodiscard]]
    constexpr std::vector<float> make_values(std::size_t const count)
    {
        std::vector<float> values;
        for (std::size_t i = 0; i < count; ++i)
        {
            values.push_back(static_cast<float>(i) * 0.5f - 7.25f);
        }
        return values;
    }

    // Copies received bytes into the decoder's own buffer, like a serial read would.
    void feed(test_decoder& dec, std::span<std::uint8_t const> const data)
    {
        auto const dst = dec.write_span();
        REQUIRE(dst.size() >= data.size());

        std::ranges::copy(data, dst.begin());
        dec.commit(data.size());
    }

    // Feeds a complete frame and returns the first message the decoder finds.
    // The returned view lives in the decoder: it stays valid until the next call on `dec`.
    [[nodiscard]]
    std::optional<message_view> decode_one(test_decoder& dec, std::span<std::uint8_t const> const data)
    {
        feed(dec, data);
        return dec.next();
    }

    // Builds a frame by hand, for encodings the encoder does not offer (or to test the decoder in isolation).
    // Byte 4 is the header checksum (xor8 over bytes 0..3), the last byte is the payload checksum.
    [[nodiscard]]
    Bytes make_frame(message_type const type, value_encoding const encoding, Bytes const& payload)
    {
        Bytes frame(constants::message_header_length, 0u);

        encode_header(
            spq::header{
                .signature = constants::default_signature,
                .control = helper::make_control_byte(type, encoding),
                .payload_length = static_cast<MessageLengthType>(payload.size()),
                .checksum = 0u},
            frame);

        frame[4] = helper::xor8(std::span{frame}.first(4u));

        frame.insert(frame.end(), payload.begin(), payload.end());
        frame.push_back(helper::xor8(payload));
        return frame;
    }
}

TEST_CASE("XOR8 checksum is calculated correctly")
{
    auto const [input, expected] = GENERATE(
        table<Bytes, std::uint8_t>({
            {                            Bytes{}, 0x00},
            {                        Bytes{0x00}, 0x00},
            {                        Bytes{0xFF}, 0xFF},
            {                  Bytes{0x01, 0x02}, 0x03},
            {                  Bytes{0xFF, 0xFF}, 0x00},
            {                  Bytes{0xAA, 0x55}, 0xFF},
            {            Bytes{0x12, 0x34, 0x56}, 0x70},
            {      Bytes{0x01, 0x02, 0x04, 0x08}, 0x0F},
            {      Bytes{0xDE, 0xAD, 0xBE, 0xEF}, 0x22},
            {Bytes{0x01, 0x02, 0x03, 0x04, 0x05}, 0x01},
    }));
    CAPTURE(input, expected);

    CHECK(helper::xor8(input) == expected);
}

TEST_CASE("Single id/value pair survives an encode/decode round trip")
{
    auto const id = GENERATE(as<std::uint8_t>{}, 0, 1, 42, 255);
    auto const value = GENERATE(
        as<float>{},
        0.0f,
        -0.0f,
        1.0f,
        -1.5f,
        3.14159f,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::denorm_min(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN());
    CAPTURE(static_cast<int>(id), value);

    constexpr encoder enc{};

    auto const message = enc.encode_value_pair(fresh_buffer(), id, value);
    REQUIRE(message);

    constexpr auto expected_length = constants::message_header_length + constants::bytes_per_value_pair + constants::checksum_length;
    CHECK(message.size() == expected_length);

    test_decoder dec;
    auto const msg = decode_one(dec, message.data);
    REQUIRE(msg.has_value());

    auto const& view = *msg;
    CHECK(view.type() == message_type::id_value_pair);
    CHECK(view.header.encoding() == value_encoding::floating_point);
    CHECK(view.header.checksum_enabled());
    REQUIRE(view.value_count() == 1u);

    auto const [decoded_id, decoded_value] = view.sample_at(0u);
    CHECK(decoded_id == id);
    CHECK(same_value(value, decoded_value));
}

TEST_CASE("Multiple id/value pairs survive an encode/decode round trip")
{
    auto const count = GENERATE(as<std::size_t>{}, 0, 1, 2, 8, 100);
    CAPTURE(count);

    auto const ids = make_ids(count);
    auto const values = make_values(count);

    constexpr encoder enc{};

    auto const message = enc.encode_value_pairs(fresh_buffer(), ids, values);
    REQUIRE(message);

    auto const expected_length = constants::message_header_length + count * constants::bytes_per_value_pair + constants::checksum_length;
    CHECK(message.size() == expected_length);

    test_decoder dec;
    auto const msg = decode_one(dec, message.data);
    REQUIRE(msg.has_value());

    auto const& view = *msg;
    CHECK(view.type() == message_type::id_value_pair);
    REQUIRE(view.value_count() == count);

    std::size_t i = 0;
    for (auto const [decoded_id, decoded_value] : view.samples())
    {
        CAPTURE(i);
        CHECK(decoded_id == ids[i]);
        CHECK(same_value(values[i], decoded_value));
        ++i;
    }
    CHECK(i == count);
}

TEST_CASE("Bulk single-id message survives an encode/decode round trip")
{
    auto const id = GENERATE(as<std::uint8_t>{}, 0, 7, 255);
    auto const count = GENERATE(as<std::size_t>{}, 0, 1, 4, 100);
    CAPTURE(static_cast<int>(id), count);

    auto const values = make_values(count);

    constexpr encoder enc{};

    auto const message = enc.encode_bulk_single_id(fresh_buffer(), id, values);
    REQUIRE(message);

    auto const expected_length = constants::message_header_length + 1u + count * constants::bytes_per_value + constants::checksum_length;
    CHECK(message.size() == expected_length);

    test_decoder dec;
    auto const msg = decode_one(dec, message.data);
    REQUIRE(msg.has_value());

    auto const& view = *msg;
    CHECK(view.type() == message_type::bulk_single_id);
    REQUIRE(view.value_count() == count);

    std::size_t i = 0;
    for (auto const [decoded_id, decoded_value] : view.samples())
    {
        CAPTURE(i);
        CHECK(decoded_id == id);
        CHECK(same_value(values[i], decoded_value));
        ++i;
    }
    CHECK(i == count);
}

TEST_CASE("Value encodings are decoded to the right number")
{
    // A bulk message with one value has the same payload layout as a single pair: [id][u32].
    auto const type = GENERATE(message_type::id_value_pair, message_type::bulk_single_id);
    auto const [encoding, raw_bits, expected] = GENERATE(
        table<value_encoding, std::uint32_t, double>({
            {  value_encoding::floating_point, 0x3FC0'0000u,           1.5},
            {  value_encoding::signed_integer, 0xFFFF'FFFFu,          -1.0},
            {  value_encoding::signed_integer, 0x8000'0000u, -2147483648.0},
            {  value_encoding::signed_integer, 0x7FFF'FFFFu,  2147483647.0},
            {value_encoding::unsigned_integer, 0x0000'0001u,           1.0},
            {value_encoding::unsigned_integer, 0xFFFF'FFFFu,  4294967295.0},
    }));
    CAPTURE(static_cast<int>(type), static_cast<int>(encoding), raw_bits, expected);

    Bytes payload(1u + constants::bytes_per_value);
    payload[0] = 9u;
    helper::write_u32(payload.data() + 1u, raw_bits);

    auto const frame = make_frame(type, encoding, payload);

    test_decoder dec;
    auto const msg = decode_one(dec, frame);
    REQUIRE(msg.has_value());

    CHECK(msg->header.encoding() == encoding);
    REQUIRE(msg->value_count() == 1u);

    auto const [decoded_id, decoded_value] = msg->sample_at(0u);
    CHECK(decoded_id == 9u);
    CHECK(decoded_value == expected);
}

TEST_CASE("String message survives an encode/decode round trip")
{
    auto const text = GENERATE(
        as<std::string>{},
        std::string{},
        std::string{"a"},
        std::string{"Hello, world!"},
        std::string{"a\0b", 3},
        std::string{"\xFF\xFF ok"},
        std::string(500, 'x'));
    CAPTURE(text.size());

    constexpr encoder enc{};

    auto const message = enc.encode_string(fresh_buffer(), text);
    REQUIRE(message);

    auto const expected_length = constants::message_header_length + text.size() + constants::checksum_length;
    CHECK(message.size() == expected_length);

    test_decoder dec;
    auto const msg = decode_one(dec, message.data);
    REQUIRE(msg.has_value());

    CHECK(msg->type() == message_type::string);
    CHECK(msg->value_count() == 0u);

    auto const decoded = msg->string();
    REQUIRE(decoded.has_value());
    CHECK(std::string{*decoded} == text);
}

TEST_CASE("Command message survives an encode/decode round trip")
{
    auto const command = GENERATE(
        as<sender_command>{},
        sender_command::clear_console,
        sender_command::clear_all_datasets,
        sender_command::clear_single_dataset,
        sender_command::delete_all_datasets,
        sender_command::delete_single_dataset,
        sender_command::set_dataset_name,
        sender_command::switch_plot_type);
    auto const data = GENERATE(
        as<Bytes>{},
        Bytes{},
        Bytes{0x01},
        Bytes{0x61, 0x62, 0x63},
        Bytes{0xFF, 0x00, 0xFF});
    CAPTURE(static_cast<int>(command), data);

    encoder enc{};
    auto const message = enc.encode_command(fresh_buffer(), command, data);
    REQUIRE(message);

    auto const expected_length = constants::message_header_length + 1u + data.size() + constants::checksum_length;
    CHECK(message.size() == expected_length);

    test_decoder dec;
    auto const msg = decode_one(dec, message.data);
    REQUIRE(msg.has_value());

    CHECK(msg->type() == message_type::command);
    CHECK(msg->command() == command);
    CHECK(std::ranges::equal(msg->command_data(), data));
}

TEST_CASE("Encoded frames match the wire format byte for byte")
{
    encoder enc{};

    SECTION("id/value pair")
    {
        auto const message = enc.encode_value_pair(fresh_buffer(), 0x01, 1.0f);
        Bytes const expected{0xFF, 0x40, 0x05, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x80, 0x3F, 0xBE};
        CHECK(to_bytes(message) == expected);
    }

    SECTION("bulk single id")
    {
        constexpr std::array values{1.0f};
        auto const message = enc.encode_bulk_single_id(fresh_buffer(), 0x02, values);
        Bytes const expected{0xFF, 0x48, 0x05, 0x00, 0xB2, 0x02, 0x00, 0x00, 0x80, 0x3F, 0xBD};
        CHECK(to_bytes(message) == expected);
    }

    SECTION("string")
    {
        auto const message = enc.encode_string(fresh_buffer(), "AB");
        Bytes const expected{0xFF, 0x44, 0x02, 0x00, 0xB9, 0x41, 0x42, 0x03};
        CHECK(to_bytes(message) == expected);
    }

    SECTION("command without data")
    {
        auto const message = enc.encode_command(fresh_buffer(), sender_command::clear_console);
        Bytes const expected{0xFF, 0x4C, 0x01, 0x00, 0xB2, 0x00, 0x00};
        CHECK(to_bytes(message) == expected);
    }
}

TEST_CASE("Custom signature is honoured by encoder and decoder")
{
    encoder enc{};
    auto const message = enc.encode_command(fresh_buffer(), sender_command::clear_console, {}, 0xA5);
    REQUIRE(message);
    CHECK(message.data[0] == 0xA5);

    SECTION("matching decoder accepts the frame")
    {
        test_decoder dec{0xA5};
        auto const msg = decode_one(dec, message.data);
        REQUIRE(msg.has_value());
        CHECK(msg->command() == sender_command::clear_console);
    }

    SECTION("decoder with the default signature ignores the frame")
    {
        test_decoder dec;
        CHECK_FALSE(decode_one(dec, message.data).has_value());
    }

    SECTION("decode_next treats a frame with another signature as garbage")
    {
        auto const out = decode_next(message.data);
        CHECK(out.result == decode_result::need_more_data);
        CHECK(out.consumed == message.size());
    }
}

TEST_CASE("Encoder rejects invalid input")
{
    constexpr encoder enc{};

    SECTION("buffer smaller than the frame")
    {
        static constexpr std::size_t frame_size = constants::message_header_length + constants::bytes_per_value_pair + constants::checksum_length;
        auto const size = GENERATE(range(std::size_t{0}, frame_size));
        CAPTURE(size);

        auto const message = enc.encode_value_pair(std::span{test_buffer}.first(size), 1, 1.0f);
        CHECK_FALSE(message);
    }

    SECTION("ids and values differ in length")
    {
        Bytes const ids{1, 2};
        std::vector const values{1.0f};

        CHECK_FALSE(enc.encode_value_pairs(fresh_buffer(), ids, values));
    }
}

TEST_CASE("decode_next reports how many bytes may be dropped")
{
    encoder enc{};
    auto const frame = to_bytes(enc.encode_string(fresh_buffer(), "abc"));

    SECTION("complete frame")
    {
        auto const out = decode_next(frame);
        CHECK(out.result == decode_result::message_available);
        CHECK(out.consumed == frame.size());
        CHECK(out.message.string() == std::string_view{"abc"});
    }

    SECTION("garbage in front of a complete frame is included")
    {
        Bytes bytes{0x00, 0x01};
        bytes.insert(bytes.end(), frame.begin(), frame.end());

        auto const out = decode_next(bytes);
        CHECK(out.result == decode_result::message_available);
        CHECK(out.consumed == bytes.size());
        CHECK(out.message.string() == std::string_view{"abc"});
    }

    SECTION("partial frame keeps all of its bytes")
    {
        auto const out = decode_next(std::span{frame}.first(frame.size() - 1u));
        CHECK(out.result == decode_result::need_more_data);
        CHECK(out.consumed == 0u);
    }

    SECTION("only the signature byte is available")
    {
        auto const out = decode_next(std::span{frame}.first(1u));
        CHECK(out.result == decode_result::need_more_data);
        CHECK(out.consumed == 0u);
    }

    SECTION("bytes without a signature are dropped entirely")
    {
        Bytes const garbage{0x00, 0x01, 0x02};

        auto const out = decode_next(garbage);
        CHECK(out.result == decode_result::need_more_data);
        CHECK(out.consumed == garbage.size());
    }

    SECTION("a bad checksum drops exactly one byte")
    {
        auto corrupted = frame;
        corrupted.back() ^= 0x01;

        auto const out = decode_next(corrupted);
        CHECK(out.result == decode_result::invalid);
        CHECK(out.consumed == 1u);
    }

    SECTION("a malformed payload is invalid even with a matching checksum")
    {
        // 3 payload bytes are not a multiple of 5 (id + float32)
        auto const bad = make_frame(message_type::id_value_pair, value_encoding::floating_point, Bytes{1, 2, 3});

        auto const out = decode_next(bad);
        CHECK(out.result == decode_result::invalid);
        CHECK(out.consumed == 1u);
    }
}

TEST_CASE("Decoder handles imperfect streams")
{
    encoder enc{};

    SECTION("frame delivered one byte at a time")
    {
        auto const message = enc.encode_string(fresh_buffer(), "streaming");
        REQUIRE(message);

        test_decoder dec;
        for (std::size_t i = 0; i + 1u < message.size(); ++i)
        {
            feed(dec, message.data.subspan(i, 1u));
            REQUIRE_FALSE(dec.next().has_value());
        }

        feed(dec, message.data.last(1u));
        auto const msg = dec.next();
        REQUIRE(msg.has_value());
        CHECK(msg->string() == std::string_view{"streaming"});
    }

    SECTION("garbage before the signature is skipped")
    {
        auto bytes = Bytes{0x00, 0x12, 0x34};
        auto const [data] = enc.encode_command(fresh_buffer(), sender_command::switch_plot_type);
        bytes.insert(bytes.end(), data.begin(), data.end());

        test_decoder dec;
        feed(dec, bytes);

        auto const msg = dec.next();
        REQUIRE(msg.has_value());
        CHECK(msg->command() == sender_command::switch_plot_type);
    }

    SECTION("corrupted checksum is rejected and the decoder recovers")
    {
        auto corrupted = to_bytes(enc.encode_string(fresh_buffer(), "payload"));
        corrupted.back() ^= 0x01;

        test_decoder dec;
        feed(dec, corrupted);
        CHECK_FALSE(dec.next().has_value());

        auto const good = to_bytes(enc.encode_string(fresh_buffer(), "payload"));
        feed(dec, good);

        auto const msg = dec.next();
        REQUIRE(msg.has_value());
        CHECK(msg->string() == std::string_view{"payload"});
    }

    SECTION("corrupted payload is rejected")
    {
        auto corrupted = to_bytes(enc.encode_string(fresh_buffer(), "payload"));
        corrupted[constants::message_header_length] ^= 0x10;

        test_decoder dec;
        CHECK_FALSE(decode_one(dec, corrupted).has_value());
    }

    SECTION("a false signature does not swallow the frame behind it")
    {
        // Looks like the start of a 2-byte string frame, but its checksum cannot match.
        // Only this single byte may be dropped, otherwise the real frame behind it is lost.
        Bytes bytes{0xFF, helper::make_control_byte(message_type::string), 0x02, 0x00, 0x00};

        auto const real = to_bytes(enc.encode_string(fresh_buffer(), "hello"));
        bytes.insert(bytes.end(), real.begin(), real.end());

        test_decoder dec;
        feed(dec, bytes);

        auto const msg = dec.next();
        REQUIRE(msg.has_value());
        CHECK(msg->string() == std::string_view{"hello"});
    }

    SECTION("several frames in one chunk are returned one by one")
    {
        auto stream = to_bytes(enc.encode_string(fresh_buffer(), "first"));
        auto const second = to_bytes(enc.encode_value_pair(fresh_buffer(), 3, 2.5f));
        stream.insert(stream.end(), second.begin(), second.end());

        test_decoder dec;
        feed(dec, stream);

        auto const first_msg = dec.next();
        REQUIRE(first_msg.has_value());
        CHECK(first_msg->string() == std::string_view{"first"});

        auto const second_msg = dec.next(); // first_msg must not be used after this call
        REQUIRE(second_msg.has_value());
        CHECK(second_msg->type() == message_type::id_value_pair);
        REQUIRE(second_msg->value_count() == 1u);

        auto const [decoded_id, decoded_value] = second_msg->sample_at(0u);
        CHECK(decoded_id == 3u);
        CHECK(decoded_value == 2.5);

        CHECK_FALSE(dec.next().has_value());
    }
}

TEST_CASE("Decoder survives a long stream delivered in odd-sized chunks")
{
    // 2000 frames * 11 bytes is far more than the decoder buffer holds, and a chunk size of 7
    // splits almost every frame, so this exercises partial frames, consume and compaction.
    constexpr std::size_t frame_count = 2000u;
    constexpr std::size_t chunk_size = 7u;

    encoder enc{};

    Bytes stream;
    for (std::size_t i = 0; i < frame_count; ++i)
    {
        auto const message = enc.encode_value_pair(
            fresh_buffer(),
            static_cast<std::uint8_t>(i),
            static_cast<float>(i) * 0.25f);
        REQUIRE(message);
        stream.insert(stream.end(), message.data.begin(), message.data.end());
    }
    REQUIRE(stream.size() > 4096u);

    test_decoder dec;
    std::vector<sample> received;

    for (std::size_t offset = 0; offset < stream.size(); offset += chunk_size)
    {
        auto const length = std::min(chunk_size, stream.size() - offset);
        feed(dec, std::span<std::uint8_t const>{stream}.subspan(offset, length));

        while (auto const msg = dec.next())
        {
            REQUIRE(msg->value_count() == 1u);
            received.push_back(msg->sample_at(0u));
        }
    }

    REQUIRE(received.size() == frame_count);
    for (std::size_t i = 0; i < frame_count; ++i)
    {
        CAPTURE(i);
        CHECK(received[i].id == static_cast<std::uint8_t>(i));
        CHECK(received[i].value == static_cast<double>(static_cast<float>(i) * 0.25f));
    }
}

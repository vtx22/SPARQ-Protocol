#include "catch2/generators/catch_generators_range.hpp"

#include <sparq/decoder.hpp>
#include <sparq/encoder.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace spq;

namespace
{
    using Bytes = std::vector<std::uint8_t>;

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

    decoder dec;
    REQUIRE(dec.consume(message.data) == decode_result::message_available);

    auto const view = dec.message();
    CHECK(view.type() == message_type::id_value_pair);
    CHECK(view.header.encoding() == value_encoding::floating_point);
    CHECK(view.header.checksum_enabled());
    REQUIRE(view.value_count() == 1u);
    CHECK(view.value_id(0) == id);

    auto const decoded = view.value(0);
    REQUIRE(decoded.has_value());
    CHECK(same_bits(*decoded, value));
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

    decoder dec;
    REQUIRE(dec.consume(message.data) == decode_result::message_available);

    auto const view = dec.message();
    CHECK(view.type() == message_type::id_value_pair);
    REQUIRE(view.value_count() == count);

    for (std::size_t i = 0; i < count; ++i)
    {
        CAPTURE(i);
        CHECK(view.value_id(i) == ids[i]);

        auto const decoded = view.value(i);
        REQUIRE(decoded.has_value());
        CHECK(same_bits(*decoded, values[i]));
    }
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

    decoder dec;
    REQUIRE(dec.consume(message.data) == decode_result::message_available);

    auto const view = dec.message();
    CHECK(view.type() == message_type::bulk_single_id);
    REQUIRE(view.value_count() == count);

    for (std::size_t i = 0; i < count; ++i)
    {
        CAPTURE(i);
        CHECK(view.value_id(i) == id);

        auto const decoded = view.value(i);
        REQUIRE(decoded.has_value());
        CHECK(same_bits(*decoded, values[i]));
    }
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

    decoder dec;
    REQUIRE(dec.consume(message.data) == decode_result::message_available);

    auto const view = dec.message();
    CHECK(view.type() == message_type::string);

    auto const decoded = view.string();
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

    decoder dec;
    REQUIRE(dec.consume(message.data) == decode_result::message_available);

    auto const view = dec.message();
    CHECK(view.type() == message_type::command);
    CHECK(view.command() == command);
    CHECK(std::ranges::equal(view.command_data(), data));
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
        decoder dec{0xA5};
        CHECK(dec.consume(message.data) == decode_result::message_available);
        CHECK(dec.message().command() == sender_command::clear_console);
    }

    SECTION("decoder with the default signature ignores the frame")
    {
        decoder dec;
        CHECK(dec.consume(message.data) == decode_result::need_more_data);
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

TEST_CASE("Decoder handles imperfect streams")
{
    encoder enc{};

    SECTION("frame delivered one byte at a time")
    {
        auto const message = enc.encode_string(fresh_buffer(), "streaming");
        REQUIRE(message);

        decoder dec;
        for (std::size_t i = 0; i + 1u < message.size(); ++i)
        {
            REQUIRE(dec.consume(message.data.subspan(i, 1u)) == decode_result::need_more_data);
        }

        REQUIRE(dec.consume(message.data.last(1u)) == decode_result::message_available);
        CHECK(dec.message().string() == std::string_view{"streaming"});
    }

    SECTION("garbage before the signature is skipped")
    {
        auto bytes = Bytes{0x00, 0x12, 0x34};
        auto const [data] = enc.encode_command(fresh_buffer(), sender_command::switch_plot_type);
        bytes.insert(bytes.end(), data.begin(), data.end());

        decoder dec;
        REQUIRE(dec.consume(bytes) == decode_result::message_available);
        CHECK(dec.message().command() == sender_command::switch_plot_type);
    }

    SECTION("corrupted checksum is rejected and the decoder recovers")
    {
        auto corrupted = to_bytes(enc.encode_string(fresh_buffer(), "payload"));
        corrupted.back() ^= 0x01;

        decoder dec;
        CHECK(dec.consume(corrupted) == decode_result::invalid);

        auto const good = to_bytes(enc.encode_string(fresh_buffer(), "payload"));
        REQUIRE(dec.consume(good) == decode_result::message_available);
        CHECK(dec.message().string() == std::string_view{"payload"});
    }

    SECTION("corrupted payload is rejected")
    {
        auto corrupted = to_bytes(enc.encode_string(fresh_buffer(), "payload"));
        corrupted[constants::message_header_length] ^= 0x10;

        decoder dec;
        CHECK(dec.consume(corrupted) == decode_result::invalid);
    }

    SECTION("decoder can be reused for consecutive messages")
    {
        auto const first = to_bytes(enc.encode_string(fresh_buffer(), "first"));
        auto const second = to_bytes(enc.encode_value_pair(fresh_buffer(), 3, 2.5f));

        decoder dec;
        REQUIRE(dec.consume(first) == decode_result::message_available);
        CHECK(dec.message().string() == std::string_view{"first"});

        REQUIRE(dec.consume(second) == decode_result::message_available);
        CHECK(dec.message().type() == message_type::id_value_pair);
        CHECK(dec.message().value_id(0) == 3);
        CHECK(dec.message().value(0) == 2.5f);
    }
}

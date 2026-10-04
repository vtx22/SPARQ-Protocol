#pragma once

#include "protocol.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace spq
{
    constexpr void encode_header(header const& value, std::span<std::uint8_t> data) noexcept
    {
        if (data.size() < constants::message_header_length)
        {
            return;
        }

        data[0] = value.signature;
        data[1] = value.control;

        helper::write_u16(&data[2], value.payload_length);

        data[4] = value.checksum;
    }

    struct encoded_message
    {
        std::span<std::uint8_t const> data{};

        [[nodiscard]]
        constexpr explicit operator bool() const noexcept
        {
            return !data.empty();
        }

        [[nodiscard]]
        constexpr std::size_t size() const noexcept
        {
            return data.size();
        }
    };

    class encoder
    {
    public:
        constexpr encoder() noexcept = default;

        [[nodiscard]]
        constexpr encoded_message encode_value_pair(
            std::span<std::uint8_t> const buffer,
            std::uint8_t const id,
            float const value,
            SignatureType const signature = constants::default_signature) const noexcept
        {
            return encode_value_pairs(
                buffer,
                std::span{&id, 1u},
                std::span{&value, 1u},
                signature);
        }

        [[nodiscard]]
        constexpr encoded_message encode_value_pairs(
            std::span<std::uint8_t> buffer,
            std::span<std::uint8_t const> const ids,
            std::span<float const> const values,
            SignatureType const signature = constants::default_signature) const noexcept
        {
            if (ids.size() != values.size())
            {
                return {};
            }

            auto const payload_length = ids.size() * constants::bytes_per_value_pair;

            if (!has_capacity(buffer, payload_length))
            {
                return {};
            }

            header const header{
                .signature = signature,
                .control = helper::make_control_byte(
                    message_type::id_value_pair,
                    value_encoding::floating_point,
                    true),
                .payload_length = static_cast<MessageLengthType>(payload_length),
                .checksum = 0u};

            encode_header(header, buffer);

            auto* payload = buffer.data() + constants::message_header_length;

            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                payload[0] = ids[i];
                write_float(payload + 1u, values[i]);
                payload += constants::bytes_per_value_pair;
            }

            return finalize(
                buffer.first(
                    constants::message_header_length + payload_length + constants::checksum_length),
                header);
        }

        [[nodiscard]]
        constexpr encoded_message encode_bulk_single_id(
            std::span<std::uint8_t> buffer,
            std::uint8_t const id,
            std::span<float const> const values,
            SignatureType const signature = constants::default_signature) const noexcept
        {
            auto const payload_length = 1u + values.size() * constants::bytes_per_value;

            if (!has_capacity(buffer, payload_length))
            {
                return {};
            }

            header const header{
                .signature = signature,
                .control = helper::make_control_byte(
                    message_type::bulk_single_id,
                    value_encoding::floating_point,
                    true),
                .payload_length = static_cast<MessageLengthType>(payload_length),
                .checksum = 0u};

            encode_header(header, buffer);

            auto* payload = buffer.data() + constants::message_header_length;

            payload[0] = id;

            for (std::size_t i = 0; i < values.size(); ++i)
            {
                write_float(payload + 1u + i * constants::bytes_per_value, values[i]);
            }

            return finalize(
                buffer.first(
                    constants::message_header_length + payload_length + constants::checksum_length),
                header);
        }

        [[nodiscard]]
        encoded_message encode_string(
            std::span<std::uint8_t> buffer,
            std::string_view const string,
            SignatureType const signature = constants::default_signature) const noexcept
        {
            auto const payload_length = string.size();

            if (!has_capacity(buffer, payload_length))
            {
                return {};
            }

            header const header{
                .signature = signature,
                .control = helper::make_control_byte(
                    message_type::string,
                    value_encoding::floating_point,
                    true),
                .payload_length =
                    static_cast<MessageLengthType>(payload_length),
                .checksum = 0u};

            encode_header(header, buffer);

            auto* payload = buffer.data() + constants::message_header_length;

            for (std::size_t i = 0; i < string.size(); ++i)
            {
                payload[i] = static_cast<std::uint8_t>(string[i]);
            }

            return finalize(
                buffer.first(
                    constants::message_header_length + payload_length + constants::checksum_length),
                header);
        }

        [[nodiscard]]
        encoded_message encode_command(
            std::span<std::uint8_t> buffer,
            sender_command command,
            std::span<std::uint8_t const> const command_data = {},
            SignatureType const signature = constants::default_signature) const noexcept
        {
            auto const payload_length = command_data.size() + 1u;

            if (!has_capacity(buffer, payload_length))
            {
                return {};
            }

            header const header{
                .signature = signature,
                .control = helper::make_control_byte(
                    message_type::command,
                    value_encoding::floating_point,
                    true),
                .payload_length = static_cast<MessageLengthType>(payload_length),
                .checksum = 0u};

            encode_header(header, buffer);

            auto* payload = buffer.data() + constants::message_header_length;
            payload[0] = static_cast<std::uint8_t>(command);

            for (std::size_t i = 0; i < command_data.size(); ++i)
            {
                payload[i + 1u] = command_data[i];
            }

            return finalize(
                buffer.first(
                    constants::message_header_length + payload_length + constants::checksum_length),
                header);
        }

    private:
        [[nodiscard]]
        static constexpr bool has_capacity(
            std::span<std::uint8_t> const buffer,
            std::size_t const payload_length) noexcept
        {
            if (payload_length > constants::max_payload_length)
            {
                return false;
            }

            return buffer.size() >= constants::message_header_length + payload_length + constants::checksum_length;
        }

        static constexpr void write_float(std::uint8_t* data, float const value) noexcept
        {
            auto const bits = std::bit_cast<std::uint32_t>(value);
            helper::write_u32(data, bits);
        }

        [[nodiscard]]
        static constexpr encoded_message finalize(
            std::span<std::uint8_t> data,
            header const& header_value) noexcept
        {
            auto const payload_begin = data.begin() + constants::message_header_length;
            auto const payload = std::span<std::uint8_t const>{
                payload_begin,
                header_value.payload_length};

            data[constants::message_header_length + header_value.payload_length] = helper::xor8(payload);
            return encoded_message{.data = data};
        }
    };
}

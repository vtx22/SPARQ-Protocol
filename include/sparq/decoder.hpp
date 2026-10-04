#pragma once

#include "protocol.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace spq
{
    struct value_view
    {
        std::uint8_t id{};
        float value{};
    };

    struct message_view
    {
        spq::header header{};
        std::span<std::uint8_t const> payload{};
        std::span<std::uint8_t const> raw{};

        [[nodiscard]]
        constexpr message_type type() const noexcept
        {
            return header.type();
        }

        [[nodiscard]]
        constexpr bool checksum_valid() const noexcept
        {
            auto const cs_index = constants::message_header_length + header.payload_length;
            if (raw.size() < cs_index + constants::checksum_length)
            {
                return false;
            }

            return helper::xor8(payload) == raw[cs_index];
        }

        [[nodiscard]]
        constexpr std::size_t value_count() const noexcept
        {
            switch (type())
            {
            case message_type::id_value_pair:
            {
                return payload.size() / constants::bytes_per_value_pair;
            }
            case message_type::bulk_single_id:
            {
                if (payload.empty())
                {
                    return 0;
                }

                return (payload.size() - 1) / constants::bytes_per_value;
            }
            default:
                return 0;
            }
        }

        [[nodiscard]]
        constexpr std::optional<std::uint8_t> value_id(std::size_t const index) const noexcept
        {
            if (type() == message_type::id_value_pair)
            {
                return payload[index * constants::bytes_per_value_pair];
            }

            if (type() == message_type::bulk_single_id)
            {
                return payload[0];
            }

            return std::nullopt;
        }

        [[nodiscard]]
        constexpr std::optional<float> value(std::size_t const index) const noexcept
        {
            std::size_t offset{};

            switch (type())
            {
            case message_type::id_value_pair:
                offset = index * constants::bytes_per_value_pair + 1u;
                break;
            case message_type::bulk_single_id:
                offset = index * constants::bytes_per_value + 1u;
                break;
            default:
                return std::nullopt;
            }

            auto const bits = helper::read_u32(payload.data() + offset);
            return std::bit_cast<float>(bits);
        }

        [[nodiscard]]
        constexpr std::optional<std::string_view> string() const noexcept
        {
            if (type() != message_type::string)
            {
                return std::nullopt;
            }

            return std::string_view{reinterpret_cast<char const*>(payload.data()), payload.size()};
        }

        [[nodiscard]]
        constexpr std::optional<sender_command> command() const noexcept
        {
            if (type() != message_type::command || payload.empty())
            {
                return std::nullopt;
            }

            return static_cast<sender_command>(payload[0]);
        }

        [[nodiscard]]
        constexpr std::span<std::uint8_t const> command_data() const noexcept
        {
            if (type() != message_type::command || payload.size() <= 1u)
            {
                return {};
            }

            return payload.subspan(1u);
        }
    };

    [[nodiscard]]
    constexpr header decode_header(std::span<std::uint8_t const> const data) noexcept
    {
        header result{};

        if (data.size() < constants::message_header_length)
        {
            return result;
        }

        result.signature = data[0];
        result.control = data[1];
        result.payload_length = helper::read_u16(&data[2]);
        result.checksum = data[4];

        return result;
    }

    enum class decode_result
    {
        need_more_data,
        message_available,
        invalid
    };

    class decoder
    {
    public:
        explicit constexpr decoder(
            SignatureType const signature = constants::default_signature) noexcept
            : m_signature(signature)
        {
        }

        constexpr void reset() noexcept
        {
            m_state = state::waiting_for_signature;
            m_header = {};
            m_header_bytes = 0u;
            m_payload_bytes = 0u;
        }

        [[nodiscard]]
        constexpr decode_result consume(std::span<std::uint8_t const> data) noexcept
        {
            for (auto const byte : data)
            {
                auto const result = consume_byte(byte);

                if (result != decode_result::need_more_data)
                {
                    return result;
                }
            }

            return decode_result::need_more_data;
        }

        [[nodiscard]]
        constexpr message_view message() const noexcept
        {
            return m_message;
        }

    private:
        enum class state
        {
            waiting_for_signature,
            reading_header,
            reading_payload,
            reading_checksum
        };

        [[nodiscard]]
        constexpr decode_result consume_byte(std::uint8_t const byte) noexcept
        {
            switch (m_state)
            {
            case state::waiting_for_signature:
            {
                if (byte != m_signature)
                {
                    return decode_result::need_more_data;
                }

                m_header_buffer[0] = byte;
                m_header_bytes = 1u;
                m_state = state::reading_header;
                return decode_result::need_more_data;
            }
            case state::reading_header:
            {
                m_header_buffer[m_header_bytes++] = byte;

                if (m_header_bytes < constants::message_header_length)
                {
                    return decode_result::need_more_data;
                }

                m_header = decode_header(
                    std::span<std::uint8_t const>{
                        m_header_buffer.data(),
                        m_header_buffer.size()});

                if (m_header.payload_length > constants::max_payload_length)
                {
                    reset();
                    return decode_result::invalid;
                }

                m_payload_bytes = 0u;

                if (m_header.payload_length == 0u)
                {
                    m_state = state::reading_checksum;
                }
                else
                {
                    m_state = state::reading_payload;
                }

                return decode_result::need_more_data;
            }
            case state::reading_payload:
            {
                if (m_payload_bytes >= m_payload_buffer.size())
                {
                    reset();
                    return decode_result::invalid;
                }

                m_payload_buffer[m_payload_bytes++] = byte;

                if (m_payload_bytes >= m_header.payload_length)
                {
                    m_state = state::reading_checksum;
                }

                return decode_result::need_more_data;
            }
            case state::reading_checksum:
            {
                return finish(byte);
            }
            }

            return decode_result::invalid;
        }

        [[nodiscard]]
        constexpr decode_result finish(std::uint8_t const checksum) noexcept
        {
            auto const payload = std::span<std::uint8_t const>{
                m_payload_buffer.data(),
                m_header.payload_length};

            if (m_header.checksum_enabled() && helper::xor8(payload) != checksum)
            {
                reset();
                return decode_result::invalid;
            }

            m_message = message_view{
                .header = m_header,
                .payload = payload,
                .raw = {}};

            m_state = state::waiting_for_signature;
            return decode_result::message_available;
        }

        SignatureType m_signature{constants::default_signature};
        state m_state{state::waiting_for_signature};
        header m_header{};

        std::array<std::uint8_t, constants::message_header_length> m_header_buffer{};

        static constexpr std::size_t max_payload_size = 1024u;

        std::array<std::uint8_t, max_payload_size> m_payload_buffer{};

        std::size_t m_header_bytes{};
        std::size_t m_payload_bytes{};

        message_view m_message{};
    };
}

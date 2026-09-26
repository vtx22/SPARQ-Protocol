#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>

namespace vtx::sparq
{
    using MessageLengthType = std::uint16_t;
    using SignatureType = std::uint8_t;

    namespace constants
    {
        constexpr auto protocol_endianess = std::endian::little;

        constexpr SignatureType default_signature = std::numeric_limits<SignatureType>::max();

        constexpr std::uint8_t message_header_length = 5u;
        constexpr std::uint8_t bytes_per_value_pair = 5u;
        constexpr std::uint8_t checksum_length = 1u;
        constexpr MessageLengthType max_payload_length = std::numeric_limits<MessageLengthType>::max();
        constexpr std::uint16_t min_message_length = message_header_length + 1u + checksum_length;
        constexpr std::uint32_t max_message_length = message_header_length + max_payload_length + checksum_length;
    }

    namespace helper
    {
        /**
         * @brief Checks if the system endianess is the same as the protocol endianess.
         * @return true if the system endianess is the same as the protocol endianess, false otherwise.
         */
        [[nodiscard]]
        constexpr bool system_has_protocol_endianess() noexcept
        {
            return std::endian::native == constants::protocol_endianess;
        }

        /**
         * @brief Computes the XOR checksum of a given data span.
         * @param data The span of data to compute the checksum for.
         * @param length The number of bytes to consider from the data span.
         * @return The computed XOR checksum as a uint8_t.
         */
        [[nodiscard]]
        constexpr std::uint8_t xor8_cs(std::span<std::uint8_t const> const data, std::size_t const length) noexcept
        {
            std::uint8_t cs{};

            for (auto const& b : data.first(std::min(length, data.size())))
            {
                cs ^= b;
            }

            return cs;
        }
    }
}

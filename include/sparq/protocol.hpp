#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace spq
{
    using MessageLengthType = std::uint16_t;
    using SignatureType = std::uint8_t;

    namespace constants
    {
        inline constexpr auto protocol_endianess = std::endian::little;

        inline constexpr std::size_t message_header_length = 5u;

        inline constexpr std::size_t bytes_per_value = 4u;
        inline constexpr std::size_t bytes_per_value_pair = 1u + bytes_per_value;
        inline constexpr std::size_t checksum_length = 1u;
        inline constexpr MessageLengthType max_payload_length = std::numeric_limits<MessageLengthType>::max();
        inline constexpr std::size_t min_message_length = message_header_length + checksum_length;
        inline constexpr std::size_t max_message_length = message_header_length + static_cast<std::size_t>(max_payload_length) + checksum_length;

        inline constexpr SignatureType default_signature = std::numeric_limits<SignatureType>::max();

        /*
         * Control byte layout:
         *
         * bit 0     integer values
         * bit 1     signed values
         * bit 2-3   message type
         * bit 4     reserved
         * bit 5     reserved
         * bit 6     checksum enabled
         * bit 7     reserved
         */
        inline constexpr std::uint8_t integer_values_bit = 1u << 0;
        inline constexpr std::uint8_t signed_values_bit = 1u << 1;
        inline constexpr std::uint8_t message_type_shift = 2u;
        inline constexpr std::uint8_t message_type_mask = 0b11u << message_type_shift;
        inline constexpr std::uint8_t checksum_enabled_bit = 1u << 6;
    }

    enum class message_type : std::uint8_t
    {
        id_value_pair = 0b00,
        string = 0b01,
        bulk_single_id = 0b10,
        command = 0b11
    };

    enum class sender_command : std::uint8_t
    {
        clear_console,
        clear_all_datasets,
        clear_single_dataset,
        delete_all_datasets,
        delete_single_dataset,
        set_dataset_name,
        switch_plot_type
    };

    enum class value_encoding : std::uint8_t
    {
        floating_point,
        signed_integer,
        unsigned_integer
    };

    template <typename T>
    concept wire_value = std::same_as<T, float>
                      || (std::integral<T> && sizeof(T) == sizeof(std::uint32_t));

    template <wire_value T>
    inline constexpr value_encoding encoding_of = std::floating_point<T>  ? value_encoding::floating_point
                                                : std::signed_integral<T> ? value_encoding::signed_integer
                                                                          : value_encoding::unsigned_integer;

    struct header
    {
        SignatureType signature{constants::default_signature};
        std::uint8_t control{};
        MessageLengthType payload_length{};
        std::uint8_t checksum{};

        [[nodiscard]]
        constexpr message_type type() const noexcept
        {
            return static_cast<message_type>((control & constants::message_type_mask) >> constants::message_type_shift);
        }

        [[nodiscard]]
        constexpr bool integer_values() const noexcept
        {
            return (control & constants::integer_values_bit) != 0u;
        }

        [[nodiscard]]
        constexpr bool signed_values() const noexcept
        {
            return (control & constants::signed_values_bit) != 0u;
        }

        [[nodiscard]]
        constexpr bool checksum_enabled() const noexcept
        {
            return (control & constants::checksum_enabled_bit) != 0u;
        }

        [[nodiscard]]
        constexpr value_encoding encoding() const noexcept
        {
            if (!integer_values())
            {
                return value_encoding::floating_point;
            }

            return signed_values()
                     ? value_encoding::signed_integer
                     : value_encoding::unsigned_integer;
        }
    };

    namespace helper
    {
        [[nodiscard]]
        constexpr bool system_has_protocol_endianess() noexcept
        {
            return std::endian::native == constants::protocol_endianess;
        }

        template <typename T>
            requires std::is_integral_v<T>
        [[nodiscard]]
        constexpr T correct_for_endianess(T value) noexcept
        {
            if constexpr (system_has_protocol_endianess())
            {
                return value;
            }
            else
            {
                return std::byteswap(value);
            }
        }

        [[nodiscard]]
        constexpr std::uint16_t read_u16(std::uint8_t const* data) noexcept
        {
            auto const value = static_cast<std::uint16_t>(data[0] << 0u)
                             | static_cast<std::uint16_t>(data[1] << 8u);
            return correct_for_endianess(value);
        }

        constexpr void write_u16(
            std::uint8_t* data,
            std::uint16_t const value) noexcept
        {
            auto const wire_value = correct_for_endianess(value);
            data[0] = static_cast<std::uint8_t>((wire_value >> 0u) & 0xFFu);
            data[1] = static_cast<std::uint8_t>((wire_value >> 8u) & 0xFFu);
        }

        [[nodiscard]]
        constexpr std::uint32_t read_u32(std::uint8_t const* data) noexcept
        {
            auto const value = static_cast<std::uint32_t>(data[0]) << 0u
                             | (static_cast<std::uint32_t>(data[1]) << 8u)
                             | (static_cast<std::uint32_t>(data[2]) << 16u)
                             | (static_cast<std::uint32_t>(data[3]) << 24u);
            return correct_for_endianess(value);
        }

        constexpr void write_u32(
            std::uint8_t* data,
            std::uint32_t const value) noexcept
        {
            auto const wire_value = correct_for_endianess(value);
            data[0] = static_cast<std::uint8_t>((wire_value >> 0u) & 0xFFu);
            data[1] = static_cast<std::uint8_t>((wire_value >> 8u) & 0xFFu);
            data[2] = static_cast<std::uint8_t>((wire_value >> 16u) & 0xFFu);
            data[3] = static_cast<std::uint8_t>((wire_value >> 24u) & 0xFFu);
        }

        [[nodiscard]]
        constexpr std::uint8_t xor8(std::span<std::uint8_t const> const data) noexcept
        {
            std::uint8_t checksum{};

            for (auto const byte : data)
            {
                checksum ^= byte;
            }

            return checksum;
        }

        [[nodiscard]]
        constexpr std::uint8_t make_control_byte(
            message_type const type,
            value_encoding const encoding = value_encoding::floating_point,
            bool const checksum_enabled = true) noexcept
        {
            auto control = static_cast<std::uint8_t>(static_cast<std::uint8_t>(type) << constants::message_type_shift);

            if (encoding != value_encoding::floating_point)
            {
                control |= constants::integer_values_bit;
            }

            if (encoding == value_encoding::signed_integer)
            {
                control |= constants::signed_values_bit;
            }

            if (checksum_enabled)
            {
                control |= constants::checksum_enabled_bit;
            }

            return control;
        }
    }
}

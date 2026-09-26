#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>

namespace spq
{
    constexpr std::array<std::uint8_t, 2> protocol_version = {1, 0};

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

    enum class header_control : std::uint8_t
    {
        checksum_enabled = 1 << 6,
        message_type = (1 << 2) + (1 << 3),
        signed_values = 1 << 1,
        integer_values = 1 << 0,
    };

    enum class message_type : std::uint8_t
    {
        id_value_pair = 0b00,
        string = 0b01,
        bulk_single_id = 0b10,
        command = 0b11,
    };

    enum class sender_command : std::uint8_t
    {
        clear_console,         // Remote clear the console
        clear_all_datasets,    // Remote clear all datasets data but keep the settings
        clear_single_dataset,  // Remote clear a single datasets data but keep the settings
        delete_all_datasets,   // Remote delete all datasets
        delete_single_dataset, // Remote delete single dataset
        set_dataset_name,      // Remote set the name of a given dataset
        switch_plot_type,      // Remote switch the plot type (e.g. line, heatmap, etc.)
    };

    struct message_header
    {
        SignatureType signature{};
        std::uint8_t control{};
        MessageLengthType payload_length{};
        std::uint8_t checksum{};

        message_header() = default;

        explicit message_header(uint8_t const* buffer)
        {
            decode(buffer);
        }

        constexpr void decode(uint8_t const* buffer) noexcept
        {
            signature = buffer[0];
            control = buffer[1];

            if constexpr (helper::system_has_protocol_endianess())
            {
                payload_length = (buffer[2] << 8) + buffer[3];
            }
            else
            {
                payload_length = (buffer[3] << 8) + buffer[2];
            }

            checksum = buffer[4];
        }

        constexpr void encode(uint8_t* buffer) const noexcept
        {
            buffer[0] = signature;
            buffer[1] = control;

            if constexpr (helper::system_has_protocol_endianess())
            {
                buffer[2] = payload_length >> 8;
                buffer[3] = payload_length & 0xFF;
            }
            else
            {
                buffer[3] = payload_length >> 8;
                buffer[2] = payload_length & 0xFF;
            }

            buffer[4] = checksum;
        }
    };

    struct message
    {
        message_header header;
        std::vector<uint8_t> ids;
        std::vector<double> values;
        uint16_t checksum{};
        bool valid{};
        std::string string_data{};
        message_type_t message_type{};
        sender_command_t command_type{};
        std::vector<uint8_t> command_data{};
        uint16_t nval{};

        [[nodiscard]]
        constexpr double buffer_to_double(uint8_t const* data) const noexcept
        {
            auto const msg_endian = static_cast<bool>(
                header.control & static_cast<uint8_t>(header_control::LSB_FIRST));

            uint32_t const value32 = [&] {
                uint32_t const le = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
                uint32_t const be = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];
                return (msg_endian == spq::helper::is_little_endian()) ? le : be;
            }();

            auto const is_integer = static_cast<bool>(
                header.control & static_cast<uint8_t>(header_control::INTEGER));
            auto const is_signed = static_cast<bool>(
                header.control & static_cast<uint8_t>(header_control::SIGNED));

            if (!is_integer)
            {
                return std::bit_cast<float>(value32);
            }

            if (is_signed)
            {
                return std::bit_cast<int32_t>(value32);
            }

            return value32;
        }

        constexpr void parse_msg_id_pair(uint8_t const* data)
        {
            nval = header.payload_length / SPARQ_BYTES_PER_VALUE_PAIR;
            ids.resize(nval);
            values.resize(nval);

            for (uint16_t pair = 0; pair < nval; pair++)
            {
                auto const pair_index = SPARQ_MESSAGE_HEADER_LENGTH + pair * SPARQ_BYTES_PER_VALUE_PAIR;

                ids[pair] = data[pair_index];
                values[pair] = buffer_to_double(&data[pair_index + 1]);
            }
        }

        constexpr void parse_msg_bulk_single_id(uint8_t const* data)
        {
            nval = (header.payload_length - 1) / 4;

            ids.assign(nval, data[SPARQ_MESSAGE_HEADER_LENGTH]);
            values.resize(nval);

            uint8_t const* ptr = data + SPARQ_MESSAGE_HEADER_LENGTH + 1;
            for (double& value : values)
            {
                value = buffer_to_double(ptr);
                ptr += 4; // TODO: Fix magic number
            }
        }

        constexpr void parse_msg_sender_command(uint8_t const* data)
        {
            command_type = static_cast<sender_command_t>(data[SPARQ_MESSAGE_HEADER_LENGTH]);

            if (header.payload_length <= 1)
            {
                return;
            }

            auto const additional_command_payload_length = header.payload_length - 1;
            uint8_t const* first_payload_ptr = &data[SPARQ_MESSAGE_HEADER_LENGTH];

            command_data.resize(additional_command_payload_length);
            std::copy(first_payload_ptr + 1, first_payload_ptr + additional_command_payload_length + 1, command_data.begin());
        }

        constexpr void from_array(uint8_t const* data) noexcept
        {
            header.from_array(data);

            if (header.payload_length == 0)
            {
                return;
            }

            message_type = static_cast<message_type_t>(header.control >> 2 & 0b11);

            switch (message_type)
            {
            case message_type_t::STRING:
                string_data = std::string(reinterpret_cast<char const*>(&data[SPARQ_MESSAGE_HEADER_LENGTH]), header.payload_length);
                break;
            case message_type_t::ID_PAIR:
                parse_msg_id_pair(data);
                break;
            case message_type_t::BULK_SINGLE_ID:
                parse_msg_bulk_single_id(data);
                break;
            case message_type_t::SENDER_COMMAND:
                parse_msg_sender_command(data);
                break;
            default:
                break;
            }

            checksum = data[SPARQ_MESSAGE_HEADER_LENGTH + header.payload_length];
        }
    };

}

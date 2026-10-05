#pragma once

#include "protocol.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace spq
{
    template <std::size_t N>
    class rx_buffer
    {
        std::array<std::uint8_t, N> buffer{};
        std::size_t read{}, write{};

    public:
        [[nodiscard]]
        uint8_t* write_ptr()
        {
            return buffer.data() + write;
        }

        [[nodiscard]]
        std::size_t writable() const
        {
            return N - write;
        }

        void commit(std::size_t const n)
        {
            write += n;
        }

        [[nodiscard]]
        std::span<std::uint8_t const> view() const
        {
            return {buffer.data() + read, write - read};
        }

        void consume(std::size_t const n)
        {
            read += n;
            if (read == write)
            {
                read = write = 0;
            }
        }

        void compact()
        {
            if (read == 0)
            {
                return;
            }

            std::memmove(buffer.data(), buffer.data() + read, write - read);
            write -= read;
            read = 0;
        }
    };

    struct sample
    {
        std::uint8_t id{};
        double value{};
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
        constexpr sample sample_at(std::size_t const index) const noexcept
        {
            if (type() == message_type::id_value_pair)
            {
                auto const* p = payload.data() + index * constants::bytes_per_value_pair;
                return {p[0], decode_value(helper::read_u32(p + 1u))};
            }

            auto const* p = payload.data() + 1u + index * constants::bytes_per_value;
            return {
                .id = payload[0],
                .value = decode_value(helper::read_u32(p))};
        }

        [[nodiscard]]
        constexpr auto samples() const noexcept
        {
            return std::views::iota(std::size_t{0}, value_count())
                 | std::views::transform([self = *this](std::size_t const i) {
                       return self.sample_at(i);
                   });
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

    private:
        [[nodiscard]]
        constexpr double decode_value(std::uint32_t const bits) const noexcept
        {
            switch (header.encoding())
            {
            case value_encoding::signed_integer:
                return std::bit_cast<std::int32_t>(bits);
            case value_encoding::unsigned_integer:
                return bits;
            case value_encoding::floating_point:
            default:
                return std::bit_cast<float>(bits);
            }
        }
    };

    enum class decode_result : std::uint8_t
    {
        need_more_data,
        message_available,
        invalid
    };

    struct decode_output
    {
        decode_result result{decode_result::need_more_data};
        std::size_t consumed{}; // bytes the caller may drop (garbage + frame)
        message_view message{}; // only valid if result == message_available
    };

    namespace detail
    {
        // Makes sure value()/value_id() can never read out of bounds.
        [[nodiscard]]
        constexpr bool payload_well_formed(message_type const type, std::size_t const size) noexcept
        {
            switch (type)
            {
            case message_type::id_value_pair:
                return size % constants::bytes_per_value_pair == 0;
            case message_type::bulk_single_id:
                return size >= 1u && (size - 1u) % constants::bytes_per_value == 0;
            case message_type::command:
                return size >= 1u;
            case message_type::string:
                return true;
            }
            return false;
        }
    }

    [[nodiscard]]
    inline decode_output decode_next(
        std::span<std::uint8_t const> in,
        SignatureType const signature = constants::default_signature) noexcept
    {
        // 1. signature: skip garbage up to the next candidate
        auto const* sig = static_cast<std::uint8_t const*>(std::memchr(in.data(), signature, in.size()));
        if (sig == nullptr)
        {
            return {decode_result::need_more_data, in.size(), {}}; // all garbage
        }

        auto const skip = static_cast<std::size_t>(sig - in.data());
        in = in.subspan(skip);

        // 2. header
        if (in.size() < constants::message_header_length)
        {
            return {decode_result::need_more_data, skip, {}};
        }

        auto const hdr = decode_header(in);

        // 3. payload + trailing checksum
        auto const frame_size = constants::message_header_length
                              + static_cast<std::size_t>(hdr.payload_length)
                              + constants::checksum_length;

        if (in.size() < frame_size)
        {
            return {decode_result::need_more_data, skip, {}};
        }

        message_view const msg{
            hdr,
            in.subspan(constants::message_header_length, hdr.payload_length),
            in.first(frame_size)};

        // false signature => drop only that one byte and rescan, never the whole "frame"
        if ((hdr.checksum_enabled() && !msg.checksum_valid())
            || !detail::payload_well_formed(hdr.type(), hdr.payload_length))
        {
            return {decode_result::invalid, skip + 1u, {}};
        }

        return {decode_result::message_available, skip + frame_size, msg};
    }

    template <std::size_t BufferSize, std::size_t MaxMessageLength = constants::max_message_length>
    class decoder
    {
        static_assert(BufferSize >= MaxMessageLength * 2, "buffer must hold at least two maximum-size frames");

    public:
        explicit decoder(SignatureType const signature = constants::default_signature) noexcept
            : m_signature{signature}
        {
        }

        [[nodiscard]]
        std::optional<message_view> next() noexcept
        {
            release();

            for (;;)
            {
                auto const out = decode_next(m_rx.view());

                if (out.result == decode_result::message_available)
                {
                    m_pending = out.consumed; // dropped lazily, view stays valid
                    return out.message;
                }

                m_rx.consume(out.consumed); // garbage / false signature

                if (out.result == decode_result::need_more_data)
                {
                    return std::nullopt;
                }
                // invalid: rescan what is left
            }
        }

        [[nodiscard]]
        std::span<std::uint8_t> write_span() noexcept
        {
            release();

            if (m_rx.writable() < constants::max_message_length)
            {
                m_rx.compact();
            }

            return {m_rx.write_ptr(), m_rx.writable()};
        }

        void commit(std::size_t const n) noexcept
        {
            m_rx.commit(n);
        }

    private:
        void release() noexcept
        {
            m_rx.consume(std::exchange(m_pending, 0u));
        }

        rx_buffer<BufferSize> m_rx{};
        std::size_t m_pending{};
        SignatureType m_signature;
    };
}

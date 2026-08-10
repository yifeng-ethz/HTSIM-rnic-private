#ifndef RNIC_WIDE_INTEGER_H
#define RNIC_WIDE_INTEGER_H

#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(_MSC_VER) && !defined(__clang__)

// MSVC does not provide GCC/Clang's unsigned __int128 type.  The RNIC models
// use 128-bit integers to keep rate, time, and service-debt arithmetic exact,
// so narrowing those calculations to uint64_t is not an acceptable Windows
// fallback.  This small unsigned type implements the operations used by the
// models.  Multiplication is modulo 2^128, just like the compiler intrinsic;
// callers retain their existing checked-overflow guards.
class RnicWideInteger {
public:
    constexpr RnicWideInteger() noexcept = default;

    template <typename Integer,
              typename std::enable_if<std::is_integral<Integer>::value, int>::type = 0>
    constexpr RnicWideInteger(Integer value) noexcept
        : high_(std::is_signed<Integer>::value && value < 0
                    ? std::numeric_limits<std::uint64_t>::max()
                    : 0),
          low_(static_cast<std::uint64_t>(value)) {}

    static constexpr RnicWideInteger fromHalves(std::uint64_t high,
                                                 std::uint64_t low) noexcept {
        return RnicWideInteger(high, low);
    }

    template <typename Integer,
              typename std::enable_if<std::is_integral<Integer>::value, int>::type = 0>
    explicit constexpr operator Integer() const noexcept {
        return static_cast<Integer>(low_);
    }

    friend constexpr bool operator==(RnicWideInteger lhs,
                                     RnicWideInteger rhs) noexcept {
        return lhs.high_ == rhs.high_ && lhs.low_ == rhs.low_;
    }
    friend constexpr bool operator!=(RnicWideInteger lhs,
                                     RnicWideInteger rhs) noexcept {
        return !(lhs == rhs);
    }
    friend constexpr bool operator<(RnicWideInteger lhs,
                                    RnicWideInteger rhs) noexcept {
        return lhs.high_ < rhs.high_
               || (lhs.high_ == rhs.high_ && lhs.low_ < rhs.low_);
    }
    friend constexpr bool operator>(RnicWideInteger lhs,
                                    RnicWideInteger rhs) noexcept {
        return rhs < lhs;
    }
    friend constexpr bool operator<=(RnicWideInteger lhs,
                                     RnicWideInteger rhs) noexcept {
        return !(rhs < lhs);
    }
    friend constexpr bool operator>=(RnicWideInteger lhs,
                                     RnicWideInteger rhs) noexcept {
        return !(lhs < rhs);
    }

    friend constexpr RnicWideInteger operator+(RnicWideInteger lhs,
                                               RnicWideInteger rhs) noexcept {
        const std::uint64_t low = lhs.low_ + rhs.low_;
        return fromHalves(lhs.high_ + rhs.high_ + (low < lhs.low_), low);
    }
    friend constexpr RnicWideInteger operator-(RnicWideInteger lhs,
                                               RnicWideInteger rhs) noexcept {
        return fromHalves(lhs.high_ - rhs.high_ - (lhs.low_ < rhs.low_),
                          lhs.low_ - rhs.low_);
    }
    friend constexpr RnicWideInteger operator~(RnicWideInteger value) noexcept {
        return fromHalves(~value.high_, ~value.low_);
    }
    friend constexpr RnicWideInteger operator|(RnicWideInteger lhs,
                                               RnicWideInteger rhs) noexcept {
        return fromHalves(lhs.high_ | rhs.high_, lhs.low_ | rhs.low_);
    }
    friend constexpr RnicWideInteger operator&(RnicWideInteger lhs,
                                               RnicWideInteger rhs) noexcept {
        return fromHalves(lhs.high_ & rhs.high_, lhs.low_ & rhs.low_);
    }

    friend constexpr RnicWideInteger operator<<(RnicWideInteger value,
                                                unsigned shift) noexcept {
        if (shift >= 128) {
            return {};
        }
        if (shift >= 64) {
            return fromHalves(value.low_ << (shift - 64), 0);
        }
        if (shift == 0) {
            return value;
        }
        return fromHalves((value.high_ << shift) | (value.low_ >> (64 - shift)),
                          value.low_ << shift);
    }
    friend constexpr RnicWideInteger operator>>(RnicWideInteger value,
                                                unsigned shift) noexcept {
        if (shift >= 128) {
            return {};
        }
        if (shift >= 64) {
            return fromHalves(0, value.high_ >> (shift - 64));
        }
        if (shift == 0) {
            return value;
        }
        return fromHalves(value.high_ >> shift,
                          (value.low_ >> shift) | (value.high_ << (64 - shift)));
    }

    friend constexpr RnicWideInteger operator*(RnicWideInteger lhs,
                                               RnicWideInteger rhs) noexcept {
        const Product64 low_product = multiply64(lhs.low_, rhs.low_);
        const std::uint64_t high = low_product.high
                                   + lhs.low_ * rhs.high_
                                   + lhs.high_ * rhs.low_;
        return fromHalves(high, low_product.low);
    }

    friend constexpr RnicWideInteger operator/(RnicWideInteger dividend,
                                               RnicWideInteger divisor) noexcept {
        return divide(dividend, divisor, false);
    }
    friend constexpr RnicWideInteger operator%(RnicWideInteger dividend,
                                               RnicWideInteger divisor) noexcept {
        return divide(dividend, divisor, true);
    }

    constexpr RnicWideInteger& operator+=(RnicWideInteger rhs) noexcept {
        *this = *this + rhs;
        return *this;
    }
    constexpr RnicWideInteger& operator-=(RnicWideInteger rhs) noexcept {
        *this = *this - rhs;
        return *this;
    }
    constexpr RnicWideInteger& operator|=(RnicWideInteger rhs) noexcept {
        *this = *this | rhs;
        return *this;
    }
    constexpr RnicWideInteger& operator<<=(unsigned shift) noexcept {
        *this = *this << shift;
        return *this;
    }
    constexpr RnicWideInteger& operator++() noexcept {
        *this += 1;
        return *this;
    }
    constexpr RnicWideInteger operator++(int) noexcept {
        const RnicWideInteger previous = *this;
        ++*this;
        return previous;
    }

private:
    struct Product64 {
        std::uint64_t high;
        std::uint64_t low;
    };

    constexpr RnicWideInteger(std::uint64_t high, std::uint64_t low) noexcept
        : high_(high), low_(low) {}

    static constexpr Product64 multiply64(std::uint64_t lhs,
                                          std::uint64_t rhs) noexcept {
        const std::uint64_t lhs_low = static_cast<std::uint32_t>(lhs);
        const std::uint64_t lhs_high = lhs >> 32;
        const std::uint64_t rhs_low = static_cast<std::uint32_t>(rhs);
        const std::uint64_t rhs_high = rhs >> 32;

        const std::uint64_t low_low = lhs_low * rhs_low;
        const std::uint64_t first = lhs_high * rhs_low + (low_low >> 32);
        std::uint64_t middle = static_cast<std::uint32_t>(first);
        const std::uint64_t upper = first >> 32;
        middle += lhs_low * rhs_high;
        return {lhs_high * rhs_high + upper + (middle >> 32),
                (middle << 32) | static_cast<std::uint32_t>(low_low)};
    }

    static constexpr bool bit(RnicWideInteger value, unsigned index) noexcept {
        return index < 64 ? ((value.low_ >> index) & 1) != 0
                          : ((value.high_ >> (index - 64)) & 1) != 0;
    }

    static constexpr RnicWideInteger divide(RnicWideInteger dividend,
                                            RnicWideInteger divisor,
                                            bool return_remainder) noexcept {
        RnicWideInteger quotient;
        RnicWideInteger remainder;
        if (divisor == 0) {
            return {};
        }
        for (int index = 127; index >= 0; --index) {
            const bool carry = (remainder.high_ >> 63) != 0;
            remainder = remainder << 1;
            if (bit(dividend, static_cast<unsigned>(index))) {
                remainder.low_ |= 1;
            }
            if (carry || remainder >= divisor) {
                remainder -= divisor;
                if (index < 64) {
                    quotient.low_ |= UINT64_C(1) << index;
                } else {
                    quotient.high_ |= UINT64_C(1) << (index - 64);
                }
            }
        }
        return return_remainder ? remainder : quotient;
    }

    std::uint64_t high_ = 0;
    std::uint64_t low_ = 0;
};

namespace std {

template <>
class numeric_limits<RnicWideInteger> {
public:
    static constexpr bool is_specialized = true;
    static constexpr int digits = 128;
    static constexpr bool is_signed = false;
    static constexpr bool is_integer = true;
    static constexpr bool is_exact = true;
    static constexpr RnicWideInteger min() noexcept { return 0; }
    static constexpr RnicWideInteger lowest() noexcept { return 0; }
    static constexpr RnicWideInteger max() noexcept {
        return RnicWideInteger::fromHalves(UINT64_MAX, UINT64_MAX);
    }
};

}  // namespace std

#else

using RnicWideInteger = unsigned __int128;

#endif

#endif

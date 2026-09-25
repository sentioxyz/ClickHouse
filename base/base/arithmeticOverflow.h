#pragma once

#include <base/extended_types.h>
#include <base/defines.h>

#include <bit>

// NOLINTBEGIN(google-runtime-int)

namespace common
{
    /// Multiply and ignore overflow.
    template <typename T1, typename T2>
    inline auto NO_SANITIZE_UNDEFINED mulIgnoreOverflow(T1 x, T2 y)
    {
        return x * y;
    }

    template <typename T1, typename T2>
    inline auto NO_SANITIZE_UNDEFINED addIgnoreOverflow(T1 x, T2 y)
    {
        return x + y;
    }

    template <typename T1, typename T2>
    inline auto NO_SANITIZE_UNDEFINED subIgnoreOverflow(T1 x, T2 y)
    {
        return x - y;
    }

    template <typename T>
    inline auto NO_SANITIZE_UNDEFINED negateIgnoreOverflow(T x)
    {
        return -x;
    }

    template <typename T>
    inline bool addOverflow(T x, T y, T & res)
    {
        return __builtin_add_overflow(x, y, &res);
    }

    template <>
    inline bool addOverflow(int x, int y, int & res)
    {
        return __builtin_sadd_overflow(x, y, &res);
    }

    template <>
    inline bool addOverflow(long x, long y, long & res)
    {
        return __builtin_saddl_overflow(x, y, &res);
    }

    template <>
    inline bool addOverflow(long long x, long long y, long long & res)
    {
        return __builtin_saddll_overflow(x, y, &res);
    }

    template <>
    inline bool addOverflow(Int128 x, Int128 y, Int128 & res)
    {
        res = addIgnoreOverflow(x, y);
        return (y > 0 && x > std::numeric_limits<Int128>::max() - y) ||
            (y < 0 && x < std::numeric_limits<Int128>::min() - y);
    }

    template <>
    inline bool addOverflow(UInt128 x, UInt128 y, UInt128 & res)
    {
        res = addIgnoreOverflow(x, y);
        return x > std::numeric_limits<UInt128>::max() - y;
    }

    template <>
    inline bool addOverflow(Int256 x, Int256 y, Int256 & res)
    {
        res = addIgnoreOverflow(x, y);
        return (y > 0 && x > std::numeric_limits<Int256>::max() - y) ||
            (y < 0 && x < std::numeric_limits<Int256>::min() - y);
    }

    template <>
    inline bool addOverflow(UInt256 x, UInt256 y, UInt256 & res)
    {
        res = addIgnoreOverflow(x, y);
        return x > std::numeric_limits<UInt256>::max() - y;
    }

    template <>
    inline bool addOverflow(Int512 x, Int512 y, Int512 & res)
    {
        res = addIgnoreOverflow(x, y);
        return (y > 0 && x > std::numeric_limits<Int512>::max() - y) ||
            (y < 0 && x < std::numeric_limits<Int512>::min() - y);
    }

    template <>
    inline bool addOverflow(UInt512 x, UInt512 y, UInt512 & res)
    {
        res = addIgnoreOverflow(x, y);
        return x > std::numeric_limits<UInt512>::max() - y;
    }

    template <typename T>
    inline bool subOverflow(T x, T y, T & res)
    {
        return __builtin_sub_overflow(x, y, &res);
    }

    template <>
    inline bool subOverflow(int x, int y, int & res)
    {
        return __builtin_ssub_overflow(x, y, &res);
    }

    template <>
    inline bool subOverflow(long x, long y, long & res)
    {
        return __builtin_ssubl_overflow(x, y, &res);
    }

    template <>
    inline bool subOverflow(long long x, long long y, long long & res)
    {
        return __builtin_ssubll_overflow(x, y, &res);
    }

    template <>
    inline bool subOverflow(Int128 x, Int128 y, Int128 & res)
    {
        res = subIgnoreOverflow(x, y);
        return (y < 0 && x > std::numeric_limits<Int128>::max() + y) ||
            (y > 0 && x < std::numeric_limits<Int128>::min() + y);
    }

    template <>
    inline bool subOverflow(UInt128 x, UInt128 y, UInt128 & res)
    {
        res = subIgnoreOverflow(x, y);
        return x < y;
    }

    template <>
    inline bool subOverflow(Int256 x, Int256 y, Int256 & res)
    {
        res = subIgnoreOverflow(x, y);
        return (y < 0 && x > std::numeric_limits<Int256>::max() + y) ||
            (y > 0 && x < std::numeric_limits<Int256>::min() + y);
    }

    template <>
    inline bool subOverflow(UInt256 x, UInt256 y, UInt256 & res)
    {
        res = subIgnoreOverflow(x, y);
        return x < y;
    }

    template <>
    inline bool subOverflow(Int512 x, Int512 y, Int512 & res)
    {
        res = subIgnoreOverflow(x, y);
        return (y < 0 && x > std::numeric_limits<Int512>::max() + y) ||
            (y > 0 && x < std::numeric_limits<Int512>::min() + y);
    }

    template <>
    inline bool subOverflow(UInt512 x, UInt512 y, UInt512 & res)
    {
        res = subIgnoreOverflow(x, y);
        return x < y;
    }

    template <typename T>
    inline bool mulOverflow(T x, T y, T & res)
    {
        return __builtin_mul_overflow(x, y, &res);
    }

    template <typename T, typename U, typename R>
    inline bool mulOverflow(T x, U y, R & res)
    {
        // not built in type, wide integer
        if constexpr (is_big_int_v<T>  || is_big_int_v<R> || is_big_int_v<U>)
        {
            res = mulIgnoreOverflow<R>(x, y);
            return false;
        }
        else
            return __builtin_mul_overflow(x, y, &res);
    }

    template <>
    inline bool mulOverflow(int x, int y, int & res)
    {
        return __builtin_smul_overflow(x, y, &res);
    }

    template <>
    inline bool mulOverflow(long x, long y, long & res)
    {
        return __builtin_smull_overflow(x, y, &res);
    }

    template <>
    inline bool mulOverflow(long long x, long long y, long long & res)
    {
        return __builtin_smulll_overflow(x, y, &res);
    }

    /// Overflow check is not implemented for 128-bit and 256-bit integers (as upstream: Decimal128/Decimal256 multiply
    /// does not detect overflow). The 512-bit specializations below do check it: Decimal512 allows 154 digits of
    /// precision, so its multiplication and scaling can leave the Int512 range, and that must not wrap silently.

    template <>
    inline bool mulOverflow(Int128 x, Int128 y, Int128 & res)
    {
        res = mulIgnoreOverflow(x, y);
        return false;
    }

    template <>
    inline bool mulOverflow(Int256 x, Int256 y, Int256 & res)
    {
        res = mulIgnoreOverflow(x, y);
        return false;
    }

    template <>
    inline bool mulOverflow(UInt128 x, UInt128 y, UInt128 & res)
    {
        res = mulIgnoreOverflow(x, y);
        return false;
    }

    template <>
    inline bool mulOverflow(UInt256 x, UInt256 y, UInt256 & res)
    {
        res = mulIgnoreOverflow(x, y);
        return false;
    }

    namespace detail
    {
        /// The i-th least significant 64-bit limb of a 512-bit integer.
        template <typename T>
        inline UInt64 limb512(const T & v, size_t i)
        {
            static_assert(sizeof(T) == 64);
            if constexpr (std::endian::native == std::endian::little)
                return v.items[i];
            else
                return v.items[7 - i];
        }

        template <typename T>
        inline void setLimb512(T & v, size_t i, UInt64 value)
        {
            static_assert(sizeof(T) == 64);
            if constexpr (std::endian::native == std::endian::little)
                v.items[i] = value;
            else
                v.items[7 - i] = value;
        }

        /// Both values are in the Int256 range: their upper 257 bits all equal their sign bit. A product of two such values
        /// has at most 511 significant bits, so it cannot overflow Int512. Branch-free: this runs for every value of a
        /// Decimal512 multiplication or scale-up.
        ALWAYS_INLINE inline bool bothFitInt256(const Int512 & x, const Int512 & y)
        {
            const UInt64 sx = static_cast<UInt64>(static_cast<Int64>(limb512(x, 3)) >> 63);
            const UInt64 sy = static_cast<UInt64>(static_cast<Int64>(limb512(y, 3)) >> 63);
            return ((limb512(x, 4) ^ sx) | (limb512(x, 5) ^ sx) | (limb512(x, 6) ^ sx) | (limb512(x, 7) ^ sx)
                  | (limb512(y, 4) ^ sy) | (limb512(y, 5) ^ sy) | (limb512(y, 6) ^ sy) | (limb512(y, 7) ^ sy)) == 0;
        }

        ALWAYS_INLINE inline bool bothFitUInt256(const UInt512 & x, const UInt512 & y)
        {
            return (limb512(x, 4) | limb512(x, 5) | limb512(x, 6) | limb512(x, 7)
                  | limb512(y, 4) | limb512(y, 5) | limb512(y, 6) | limb512(y, 7)) == 0;
        }

        /// Exact product of the low 256 bits of a and b (4 limbs each, unsigned): 16 limb multiplications, where the
        /// generic truncated 512-bit multiplication needs about twice as many. out: 8 limbs, least significant first.
        ALWAYS_INLINE inline void mulLimbs256(const UInt64 (&a)[4], const UInt64 (&b)[4], UInt64 (&out)[8])
        {
            for (auto & l : out)
                l = 0;
            for (size_t i = 0; i < 4; ++i)
            {
                unsigned __int128 carry = 0;
                for (size_t j = 0; j < 4; ++j)
                {
                    const unsigned __int128 t = static_cast<unsigned __int128>(a[i]) * b[j] + out[i + j] + carry;
                    out[i + j] = static_cast<UInt64>(t);
                    carry = t >> 64;
                }
                out[i + 4] = static_cast<UInt64>(carry);
            }
        }

        /// x * y for x and y in the Int256 range (the product is exact in Int512): magnitudes, product, sign.
        ALWAYS_INLINE inline Int512 mulInt256Range(const Int512 & x, const Int512 & y)
        {
            UInt64 a[4];
            UInt64 b[4];
            const bool neg_a = static_cast<Int64>(limb512(x, 3)) < 0;
            const bool neg_b = static_cast<Int64>(limb512(y, 3)) < 0;
            UInt64 carry_a = neg_a;
            UInt64 carry_b = neg_b;
            for (size_t i = 0; i < 4; ++i)
            {
                /// |v| = two's complement negation of a negative v: ~v + 1
                const unsigned __int128 ta = static_cast<unsigned __int128>(neg_a ? ~limb512(x, i) : limb512(x, i)) + carry_a;
                const unsigned __int128 tb = static_cast<unsigned __int128>(neg_b ? ~limb512(y, i) : limb512(y, i)) + carry_b;
                a[i] = static_cast<UInt64>(ta);
                b[i] = static_cast<UInt64>(tb);
                carry_a = static_cast<UInt64>(ta >> 64);
                carry_b = static_cast<UInt64>(tb >> 64);
            }
            UInt64 p[8];
            mulLimbs256(a, b, p);
            Int512 res;
            UInt64 carry = neg_a != neg_b;
            for (size_t i = 0; i < 8; ++i)
            {
                const unsigned __int128 t = static_cast<unsigned __int128>(neg_a != neg_b ? ~p[i] : p[i]) + carry;
                setLimb512(res, i, static_cast<UInt64>(t));
                carry = static_cast<UInt64>(t >> 64);
            }
            return res;
        }

        ALWAYS_INLINE inline UInt512 mulUInt256Range(const UInt512 & x, const UInt512 & y)
        {
            UInt64 a[4];
            UInt64 b[4];
            for (size_t i = 0; i < 4; ++i)
            {
                a[i] = limb512(x, i);
                b[i] = limb512(y, i);
            }
            UInt64 p[8];
            mulLimbs256(a, b, p);
            UInt512 res;
            for (size_t i = 0; i < 8; ++i)
                setLimb512(res, i, p[i]);
            return res;
        }

        /// Whether res = x * y (computed with wrap-around) overflowed, for factors that do not both fit 256 bits.
        /// Out of line: rare, and it keeps the inlined fast path small.
        NO_INLINE inline bool mulOverflowInt512Slow(const Int512 & x, const Int512 & y, const Int512 & res)
        {
            if (x == 0 || y == 0)
                return false;
            /// The only product whose check below would itself overflow: min * -1.
            if ((x == -1 && y == std::numeric_limits<Int512>::min()) || (y == -1 && x == std::numeric_limits<Int512>::min()))
                return true;
            /// Without overflow the wrapped product is exact, so dividing it by one factor gives the other one back.
            return res / y != x;
        }

        NO_INLINE inline bool mulOverflowUInt512Slow(const UInt512 & x, const UInt512 & y, const UInt512 & res)
        {
            return x != 0 && res / x != y;
        }
    }

    template <>
    ALWAYS_INLINE inline bool mulOverflow(Int512 x, Int512 y, Int512 & res)
    {
        /// Factors in the Int256 range (the common case) cannot overflow (|x * y| <= 2^510), and their product is computed
        /// with a 256x256-bit multiplication, cheaper than the generic 512-bit one.
        if (likely(detail::bothFitInt256(x, y)))
        {
            res = detail::mulInt256Range(x, y);
            return false;
        }
        res = mulIgnoreOverflow(x, y);
        return detail::mulOverflowInt512Slow(x, y, res);
    }

    template <>
    ALWAYS_INLINE inline bool mulOverflow(UInt512 x, UInt512 y, UInt512 & res)
    {
        /// Factors below 2^256 cannot overflow.
        if (likely(detail::bothFitUInt256(x, y)))
        {
            res = detail::mulUInt256Range(x, y);
            return false;
        }
        res = mulIgnoreOverflow(x, y);
        return detail::mulOverflowUInt512Slow(x, y, res);
    }
}

// NOLINTEND(google-runtime-int)

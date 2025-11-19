#pragma once

#include <Core/Types.h>

#include <arrow/util/bit_stream_utils.h>
#include <arrow/util/decimal.h>
#include <algorithm>
#include <type_traits>
#include <parquet/types.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int PARQUET_EXCEPTION;
}

template <typename T> struct ToArrowDecimal;

template <> struct ToArrowDecimal<Decimal<wide::integer<128, signed>>>
{
    using ArrowDecimal = arrow::Decimal128;
};

template <> struct ToArrowDecimal<Decimal<wide::integer<256, signed>>>
{
    using ArrowDecimal = arrow::Decimal256;
};


class ParquetDataBuffer
{
private:

public:
    ParquetDataBuffer(const uint8_t * data_, UInt64 available_, UInt8 datetime64_scale_ = DataTypeDateTime64::default_scale)
        : data(reinterpret_cast<const Int8 *>(data_)), available(available_), datetime64_scale(datetime64_scale_) {}

    template <typename TValue>
    void ALWAYS_INLINE readValue(TValue & dst)
    {
        readBytes(&dst, sizeof(TValue));
    }

    void ALWAYS_INLINE readBytes(void * dst, size_t bytes)
    {
        checkAvailable(bytes);
        std::copy(data, data + bytes, reinterpret_cast<Int8 *>(dst));
        consume(bytes);
    }

    template <typename TValue, typename ParquetType>
    void ALWAYS_INLINE readValuesOfDifferentSize(TValue * dst, size_t count)
    {
        auto necessary_bytes = count * sizeof(ParquetType);
        checkAvailable(necessary_bytes);

        for (std::size_t i = 0; i < count; i++)
        {
            auto offset = i * sizeof(ParquetType);
            dst[i] = unalignedLoad<TValue>(data + offset);
        }

        consume(necessary_bytes);
    }

    void ALWAYS_INLINE readDateTime64FromInt96(DateTime64 & dst)
    {
        static const int max_scale_num = 9;
        static const UInt64 pow10[max_scale_num + 1]
            = {1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
        static const UInt64 spd = 60 * 60 * 24;
        static const UInt64 scaled_day[max_scale_num + 1]
            = {spd,
               10 * spd,
               100 * spd,
               1000 * spd,
               10000 * spd,
               100000 * spd,
               1000000 * spd,
               10000000 * spd,
               100000000 * spd,
               1000000000 * spd};

        parquet::Int96 tmp;
        readValue(tmp);
        auto decoded = parquet::DecodeInt96Timestamp(tmp);

        uint64_t scaled_nano = decoded.nanoseconds / pow10[datetime64_scale];
        dst = static_cast<Int64>(decoded.days_since_epoch * scaled_day[datetime64_scale] + scaled_nano);
    }

    /**
     * This method should only be used to read string whose elements size is small.
     * Because memcpySmallAllowReadWriteOverflow15 instead of memcpy is used according to ColumnString::indexImpl
     */
    void ALWAYS_INLINE readString(ColumnString & column, size_t cursor)
    {
        // refer to: PlainByteArrayDecoder::DecodeArrowDense in encoding.cc
        //           deserializeBinarySSE2 in SerializationString.cpp
        checkAvailable(4);
        auto value_len = ::arrow::util::SafeLoadAs<Int32>(getArrowData());
        if (unlikely(value_len < 0 || value_len > INT32_MAX - 4))
        {
            throw Exception(ErrorCodes::PARQUET_EXCEPTION, "Invalid or corrupted value_len '{}'", value_len);
        }
        consume(4);
        checkAvailable(value_len);

        auto chars_cursor = column.getChars().size();
        column.getChars().resize(chars_cursor + value_len);

        memcpySmallAllowReadWriteOverflow15(&column.getChars()[chars_cursor], data, value_len);

        column.getOffsets().data()[cursor] = column.getChars().size();
        consume(value_len);
    }

    template <is_over_big_decimal TDecimal>
    void ALWAYS_INLINE readOverBigDecimal(TDecimal * out, Int32 elem_bytes_num)
    {
        if constexpr (std::is_same_v<TDecimal, Decimal512>)
        {
            checkAvailable(elem_bytes_num);

            constexpr size_t value_size = sizeof(typename TDecimal::NativeType);
            if (elem_bytes_num <= 0 || static_cast<size_t>(elem_bytes_num) > value_size)
                throw Exception(ErrorCodes::PARQUET_EXCEPTION,
                                "Invalid decimal byte length {} for type with {}-byte storage",
                                elem_bytes_num, value_size);

            auto * value_bytes = reinterpret_cast<uint8_t *>(&out->value);
            const auto * src = getArrowData();

            const UInt8 sign_fill = (src[0] & 0x80) ? 0xFF : 0x00;
            std::fill(value_bytes, value_bytes + value_size, sign_fill);

            for (Int32 i = 0; i < elem_bytes_num; ++i)
                value_bytes[i] = src[elem_bytes_num - 1 - i];

            consume(elem_bytes_num);
        }
        else
        {
            using TArrowDecimal = typename ToArrowDecimal<TDecimal>::ArrowDecimal;

            checkAvailable(elem_bytes_num);

            // refer to: RawBytesToDecimalBytes in reader_internal.cc, Decimal128::FromBigEndian in decimal.cc
            auto status = TArrowDecimal::FromBigEndian(getArrowData(), elem_bytes_num);
            assert(status.ok());
            status.ValueUnsafe().ToBytes(reinterpret_cast<uint8_t *>(out));
            consume(elem_bytes_num);
        }
    }

private:
    const Int8 * data;
    UInt64 available;
    const UInt8 datetime64_scale;

    void ALWAYS_INLINE checkAvailable(UInt64 num)
    {
        if (unlikely(available < num))
        {
            throw Exception(ErrorCodes::PARQUET_EXCEPTION, "Consuming {} bytes while {} available", num, available);
        }
    }

    const uint8_t * ALWAYS_INLINE getArrowData() { return reinterpret_cast<const uint8_t *>(data); }

    void ALWAYS_INLINE consume(UInt64 num)
    {
        data += num;
        available -= num;
    }
};


class LazyNullMap
{
public:
    explicit LazyNullMap(UInt64 size_) : size(size_), col_nullable(nullptr) {}

    template <typename T>
    requires std::is_integral_v<T>
    void setNull(T cursor)
    {
        initialize();
        null_map[cursor] = 1;
    }

    template <typename T>
    requires std::is_integral_v<T>
    void setNull(T cursor, UInt32 count)
    {
        initialize();
        memset(null_map + cursor, 1, count);
    }

    ColumnPtr getNullableCol() { return col_nullable; }

private:
    UInt64 size;
    UInt8 * null_map;
    ColumnPtr col_nullable;

    void initialize()
    {
        if (likely(col_nullable))
        {
            return;
        }
        auto col = ColumnVector<UInt8>::create(size);
        null_map = col->getData().data();
        col_nullable = std::move(col);
        memset(null_map, 0, size);
    }
};

}

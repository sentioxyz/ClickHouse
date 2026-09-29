#include <gtest/gtest.h>

#include <Columns/ColumnsNumber.h>
#include <Core/DecimalFunctions.h>
#include <Core/Field.h>
#include <DataTypes/DataTypesDecimal.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/AggregationCommon.h>
#include <Interpreters/KeysNullMap.h>
#include <base/arithmeticOverflow.h>
#include <Common/Exception.h>
#include <Common/intExp.h>

#include <string>

/// Unit tests of the Sentio fork's 512-bit types. Decimal512 has precision and scale up to 154, but Int512 holds only
/// |v| <= 2^511 (about 6.7e153): 10^154 does not fit, so every computation that needs 10^154 must be handled exactly.
/// Expected values are written out as decimal strings, never taken from the code under test.

namespace
{
using namespace DB;

Int512 parseInt512(const std::string & s)
{
    const bool neg = !s.empty() && s[0] == '-';
    Int512 v = 0;
    for (size_t i = neg; i < s.size(); ++i)
        v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

const std::string max_text = "6703903964971298549787012499102923063739682910296196688861780721860882015036773488400937149083451713845015929093243025426876941405973284973216824503042047";
const std::string min_text = "-6703903964971298549787012499102923063739682910296196688861780721860882015036773488400937149083451713845015929093243025426876941405973284973216824503042048";

std::string toText(Decimal512 x, UInt32 scale, bool trailing_zeros = false, bool fixed = false, UInt32 length = 0)
{
    WriteBufferFromOwnString buf;
    writeText(x, scale, buf, trailing_zeros, fixed, length);
    return buf.str();
}
}

TEST(Decimal512, Int512Limits)
{
    EXPECT_EQ(std::numeric_limits<Int512>::max(), parseInt512(max_text));
    EXPECT_EQ(std::numeric_limits<Int512>::min(), parseInt512(min_text));
}

TEST(Decimal512, Exp10)
{
    Int512 p = 1;
    for (int k = 0; k <= 153; ++k)
    {
        EXPECT_EQ(common::exp10_i512(k), p) << "10^" << k;
        EXPECT_EQ(intExp10OfSize<Int512>(k), p) << "10^" << k;
        p *= 10;
    }
    /// 10^154 does not fit Int512: saturated, never the wrapped (negative) value
    EXPECT_EQ(common::exp10_i512(154), std::numeric_limits<Int512>::max());
    EXPECT_EQ(common::exp10_i512(200), std::numeric_limits<Int512>::max());
    EXPECT_EQ(common::exp10_i512(-1), 0);
}

TEST(Decimal512, MulOverflow)
{
    const Int512 max = std::numeric_limits<Int512>::max();
    const Int512 min = std::numeric_limits<Int512>::min();
    const Int512 two_255 = Int512(1) << 255;
    const Int512 two_256 = Int512(1) << 256;
    Int512 r;
    EXPECT_FALSE(common::mulOverflow(max, Int512(1), r));
    EXPECT_EQ(r, max);
    EXPECT_TRUE(common::mulOverflow(max, Int512(2), r));
    EXPECT_TRUE(common::mulOverflow(min, Int512(-1), r));
    EXPECT_FALSE(common::mulOverflow(min, Int512(1), r));
    EXPECT_FALSE(common::mulOverflow(two_255, two_255, r));
    EXPECT_EQ(r, Int512(1) << 510);
    EXPECT_TRUE(common::mulOverflow(two_256, two_255, r)); /// 2^511
    EXPECT_FALSE(common::mulOverflow(-two_256, two_255, r)); /// -2^511 = min
    EXPECT_EQ(r, min);
    EXPECT_FALSE(common::mulOverflow(Int512(0), max, r));
    EXPECT_TRUE(common::mulOverflow(parseInt512("1" + std::string(77, '0')), parseInt512("1" + std::string(77, '0')), r)); /// 10^154

    const UInt512 umax = std::numeric_limits<UInt512>::max();
    UInt512 u;
    EXPECT_FALSE(common::mulOverflow(umax, UInt512(1), u));
    EXPECT_TRUE(common::mulOverflow(umax, UInt512(2), u));
    EXPECT_FALSE(common::mulOverflow(UInt512(1) << 256, (UInt512(1) << 255), u));
    EXPECT_TRUE(common::mulOverflow(UInt512(1) << 256, UInt512(1) << 256, u)); /// 2^512
}

TEST(Decimal512, ScaleMultiplierHelpers)
{
    EXPECT_TRUE(DecimalUtils::scaleMultiplierFits<Int512>(153));
    EXPECT_FALSE(DecimalUtils::scaleMultiplierFits<Int512>(154));
    EXPECT_TRUE(DecimalUtils::scaleMultiplierFits<Int256>(76));

    const Int512 sat = DecimalUtils::scaleMultiplier<Int512>(154);
    EXPECT_TRUE(DecimalUtils::isSaturatedScaleMultiplier(sat));
    EXPECT_FALSE(DecimalUtils::isSaturatedScaleMultiplier(DecimalUtils::scaleMultiplier<Int512>(153)));

    Int512 r = 7;
    EXPECT_FALSE(DecimalUtils::mulOverflowByScale(Int512(0), sat, r));
    EXPECT_EQ(r, 0);
    EXPECT_TRUE(DecimalUtils::mulOverflowByScale(Int512(1), sat, r));
    EXPECT_TRUE(DecimalUtils::mulOverflowByScale(Int512(-1), sat, r));
    EXPECT_FALSE(DecimalUtils::mulOverflowByScale(Int512(6), DecimalUtils::scaleMultiplier<Int512>(153), r));
    EXPECT_EQ(r, parseInt512("6" + std::string(153, '0')));
    EXPECT_TRUE(DecimalUtils::mulOverflowByScale(Int512(7), DecimalUtils::scaleMultiplier<Int512>(153), r));

    EXPECT_EQ((DecimalUtils::floatScaleMultiplier<Float64, Int512>(154)), 1e154);
    EXPECT_EQ((DecimalUtils::floatScaleMultiplier<Float64, Int512>(60)), 1e60);
}

TEST(Decimal512, SplitScale154)
{
    const Decimal512 half(parseInt512("5" + std::string(153, '0'))); /// 0.5 at scale 154
    const Decimal512 min(std::numeric_limits<Int512>::min());
    const Decimal512 max(std::numeric_limits<Int512>::max());
    for (const auto & x : {half, min, max, Decimal512(-half.value), Decimal512(1)})
    {
        const auto c = DecimalUtils::split(x, 154);
        EXPECT_EQ(c.whole, 0);
        EXPECT_EQ(c.fractional, x.value);
        EXPECT_EQ(DecimalUtils::getWholePart(x, 154), 0);
        EXPECT_EQ(DecimalUtils::getFractionalPart(x, 154), x.value);
        EXPECT_EQ(DecimalUtils::convertTo<Int64>(x, 154), 0);
    }
    EXPECT_NEAR(DecimalUtils::convertTo<Float64>(half, 154), 0.5, 1e-15);
    EXPECT_NEAR(DecimalUtils::convertTo<Float64>(max, 154), 0.6703903964971298, 1e-15);
    EXPECT_NEAR(DecimalUtils::convertTo<Float64>(min, 154), -0.6703903964971298, 1e-15);

    /// scale 153: 10^153 fits, the ordinary split
    const Decimal512 six(parseInt512("6" + std::string(153, '0') ));
    EXPECT_EQ(DecimalUtils::getWholePart(six, 153), 6);
    EXPECT_EQ(DecimalUtils::getFractionalPart(six, 153), 0);
}

TEST(Decimal512, ComponentsScale154)
{
    const Int512 half = parseInt512("5" + std::string(153, '0'));
    EXPECT_EQ(DecimalUtils::decimalFromComponents<Decimal512>(Int512(0), half, 154).value, half);
    EXPECT_THROW(DecimalUtils::decimalFromComponents<Decimal512>(Int512(1), Int512(0), 154), Exception);
    Decimal512 out;
    EXPECT_FALSE(DecimalUtils::tryGetDecimalFromComponents<Decimal512>(Int512(-1), Int512(0), 154, out));
}

TEST(Decimal512, MaxWholeValue)
{
    const Int512 max = std::numeric_limits<Int512>::max();
    EXPECT_EQ(DataTypeDecimal<Decimal512>(154, 0).maxWholeValue().value, max);
    EXPECT_EQ(DataTypeDecimal<Decimal512>(154, 1).maxWholeValue().value, max / 10);
    EXPECT_EQ(DataTypeDecimal<Decimal512>(154, 154).maxWholeValue().value, 0);
    EXPECT_EQ(DataTypeDecimal<Decimal512>(153, 0).maxWholeValue().value, parseInt512(std::string(153, '9')));
    EXPECT_EQ(DataTypeDecimal<Decimal512>(154, 60).maxWholeValue().value, max / DecimalUtils::scaleMultiplier<Int512>(60));
    EXPECT_TRUE(DataTypeDecimal<Decimal512>(154, 0).canStoreWhole(Int64(5)));
    EXPECT_FALSE(DataTypeDecimal<Decimal512>(154, 154).canStoreWhole(Int64(1)));
}

TEST(Decimal512, TextScale154)
{
    const Decimal512 half(parseInt512("5" + std::string(153, '0')));
    EXPECT_EQ(toText(half, 154), "0.5");
    EXPECT_EQ(toText(Decimal512(-half.value), 154), "-0.5");
    EXPECT_EQ(toText(Decimal512(std::numeric_limits<Int512>::max()), 154), "0." + max_text);
    EXPECT_EQ(toText(Decimal512(std::numeric_limits<Int512>::min()), 154), "-0." + min_text.substr(1));
    EXPECT_EQ(toText(Decimal512(std::numeric_limits<Int512>::min()), 154, false, true, 3), "-0.670");
    EXPECT_EQ(toText(half, 154, true), "0.5" + std::string(153, '0'));
    EXPECT_EQ(toText(Decimal512(std::numeric_limits<Int512>::min()), 0), min_text);

    const DataTypeDecimal<Decimal512> t154(154, 154);
    EXPECT_EQ(t154.parseFromString("0.5").value, half.value);
    EXPECT_EQ(t154.parseFromString("-0." + min_text.substr(1)).value, std::numeric_limits<Int512>::min());
    EXPECT_THROW(t154.parseFromString("1"), Exception);
    EXPECT_THROW(t154.parseFromString("0.6703903964971298549787012499102923063739682910296196688861780721860882015036773488400937149083451713845015929093243025426876941405973284973216824503042048"), Exception);
    const DataTypeDecimal<Decimal512> t0(154, 0);
    EXPECT_EQ(t0.parseFromString(min_text).value, std::numeric_limits<Int512>::min());
    EXPECT_THROW(t0.parseFromString(max_text.substr(0, max_text.size() - 1) + "8"), Exception); /// 2^511
}

TEST(Decimal512, FieldComparisonAcrossScales)
{
    const Decimal512 half(parseInt512("5" + std::string(153, '0')));
    /// 0.5 (scale 154) against 1 (scale 0): the scale-up by 10^154 leaves Int512, the comparison is still exact
    EXPECT_TRUE(decimalLess<Decimal512>(half, Decimal512(1), 154, 0));
    EXPECT_FALSE(decimalLess<Decimal512>(Decimal512(1), half, 0, 154));
    EXPECT_FALSE(decimalEqual<Decimal512>(half, Decimal512(1), 154, 0));
    EXPECT_TRUE(decimalLess<Decimal512>(Decimal512(-1), Decimal512(-half.value), 0, 154));
    EXPECT_TRUE(decimalEqual<Decimal512>(Decimal512(0), Decimal512(0), 154, 0));
    EXPECT_TRUE(decimalLess<Decimal512>(Decimal512(std::numeric_limits<Int512>::max()), Decimal512(1), 154, 0));
    /// scale 100 against scale 0 with a scale-up that leaves Int512 (7e60 * 10^100)
    EXPECT_FALSE(decimalLess<Decimal512>(Decimal512(parseInt512("7" + std::string(60, '0'))), Decimal512(parseInt512("5" + std::string(99, '0'))), 0, 100));
}

TEST(Decimal512, NullableKeysBitmap)
{
    /// hotfix bae5ba481f3: nullable fixed keys of 29..64 bytes are packed into UInt512 with an 8-byte null bitmap
    static_assert(getBitmapSize<UInt512>() == 8);
    static_assert(std::tuple_size_v<KeysNullMap<UInt512>> == 8);
    static_assert(getBitmapSize<UInt256>() == 4);

    auto column = ColumnUInt256::create();
    column->insertValue(UInt256(0));
    ColumnRawPtrs columns{column.get()};
    Sizes sizes{32};
    KeysNullMap<UInt512> null_bitmap{};
    null_bitmap[0] = 1; /// key 0 is NULL
    KeysNullMap<UInt512> value_bitmap{};
    const UInt512 as_null = packFixed<UInt512>(0, 1, columns, sizes, null_bitmap);
    const UInt512 as_zero = packFixed<UInt512>(0, 1, columns, sizes, value_bitmap);
    EXPECT_NE(as_null, as_zero); /// NULL and 0 must not collide
}

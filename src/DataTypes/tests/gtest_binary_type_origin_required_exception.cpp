#include <gtest/gtest.h>

#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/BinaryTypeOriginRequiredException.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <IO/ReadBufferFromString.h>
#include <Common/Exception.h>

#include <memory>
#include <optional>
#include <string>

/// BinaryTypeOriginRequiredException, the refusal of an ambiguous legacy binary type index: outwardly it is the exception it always was
/// (code, message, name, every rendering), only its C++ type is its own, kept by a direct catch and by clone and rethrow. Only that refusal
/// throws it: an unknown type code, a complexity limit and truncated data stay ordinary exceptions.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int INCORRECT_DATA;
}

namespace
{

constexpr auto MESSAGE_FORMAT = "Binary type index {0:#04x} is ambiguous without a declared origin: the Decimal512 fork wrote it for a 512-bit "
                                "type, upstream does not";

/// The rendering of the current exception that the server and its logs use, without the stack trace and the version.
struct Rendering
{
    int code = 0;
    std::string message;
    std::string display_text;
    std::string current_message;
    std::string status_message;
};

template <typename E>
Rendering renderThrown()
{
    Rendering rendering;
    try
    {
        throw E(ErrorCodes::INCORRECT_DATA, MESSAGE_FORMAT, UInt64(0x39));
    }
    catch (const Exception & e)
    {
        rendering.code = e.code();
        rendering.message = e.message();
        rendering.display_text = e.displayText();
        rendering.current_message = getCurrentExceptionMessage(/*with_stacktrace=*/ false, false, /*with_extra_info=*/ false, /*with_version=*/ false);
        rendering.status_message = ExecutionStatus::fromCurrentException("", /*with_stacktrace=*/ false, /*with_version=*/ false).message;
        EXPECT_EQ(ExecutionStatus::fromCurrentException("", false, false).code, ErrorCodes::INCORRECT_DATA);
    }
    return rendering;
}

/// Decodes `bytes` as a binary type with `origin`; returns whether the refusal was a BinaryTypeOriginRequiredException, or nothing if the
/// decoding succeeded.
std::optional<bool> decodeAndClassify(const std::string & bytes, BinaryTypeOrigin origin, size_t max_complexity = 0)
{
    try
    {
        ReadBufferFromString in(bytes);
        decodeDataType(in, origin, max_complexity);
        return {};
    }
    catch (const BinaryTypeOriginRequiredException &)
    {
        return true;
    }
    catch (const Exception &)
    {
        return false;
    }
}

}

TEST(BinaryTypeOriginRequiredException, RendersAsTheOriginalException)
{
    const auto original = renderThrown<Exception>();
    const auto derived = renderThrown<BinaryTypeOriginRequiredException>();
    EXPECT_EQ(derived.code, original.code);
    EXPECT_EQ(derived.message, original.message);
    EXPECT_EQ(derived.display_text, original.display_text);
    EXPECT_EQ(derived.current_message, original.current_message);
    EXPECT_EQ(derived.status_message, original.status_message);
    EXPECT_NE(original.message.find("Binary type index 0x39 is ambiguous"), std::string::npos) << original.message;

    const BinaryTypeOriginRequiredException e(ErrorCodes::INCORRECT_DATA, MESSAGE_FORMAT, UInt64(0x39));
    const Exception plain(ErrorCodes::INCORRECT_DATA, MESSAGE_FORMAT, UInt64(0x39));
    /// Through the public virtual interface of Poco::Exception, which renderings use: DB::Exception keeps its override of className private.
    const Poco::Exception & e_as_poco = e;
    const Poco::Exception & plain_as_poco = plain;
    EXPECT_STREQ(e_as_poco.name(), plain_as_poco.name());
    EXPECT_STREQ(e_as_poco.className(), plain_as_poco.className());
    EXPECT_STREQ(e_as_poco.what(), plain_as_poco.what());
    EXPECT_EQ(e_as_poco.displayText(), plain_as_poco.displayText());
    EXPECT_EQ(getExceptionMessage(e, false), getExceptionMessage(plain, false));
}

TEST(BinaryTypeOriginRequiredException, KeepsItsTypeThroughCloneAndRethrow)
{
    const BinaryTypeOriginRequiredException e(ErrorCodes::INCORRECT_DATA, MESSAGE_FORMAT, UInt64(0x39));
    std::unique_ptr<Exception> copy(e.clone());
    bool caught = false;
    try
    {
        copy->rethrow();
    }
    catch (const BinaryTypeOriginRequiredException & rethrown)
    {
        caught = true;
        EXPECT_EQ(rethrown.code(), ErrorCodes::INCORRECT_DATA);
        EXPECT_EQ(rethrown.message(), e.message());
    }
    EXPECT_TRUE(caught);

    /// An ordinary exception is not one.
    const Exception plain(ErrorCodes::INCORRECT_DATA, MESSAGE_FORMAT, UInt64(0x39));
    std::unique_ptr<Exception> plain_copy(plain.clone());
    bool caught_as_required = false;
    bool caught_as_plain = false;
    try
    {
        plain_copy->rethrow();
    }
    catch (const BinaryTypeOriginRequiredException &)
    {
        caught_as_required = true;
    }
    catch (const Exception &)
    {
        caught_as_plain = true;
    }
    EXPECT_FALSE(caught_as_required);
    EXPECT_TRUE(caught_as_plain);
}

TEST(BinaryTypeOriginRequiredException, OnlyTheAmbiguousLegacyIndexThrowsIt)
{
    /// The legacy Decimal512 index with precision 154 and scale 2, as the fork wrote it.
    const std::string legacy_decimal512 = "\x39\x9a\x02";
    EXPECT_EQ(decodeAndClassify(legacy_decimal512, BinaryTypeOrigin::Unknown), std::optional<bool>(true));
    EXPECT_FALSE(decodeAndClassify(legacy_decimal512, BinaryTypeOrigin::SentioLegacyV0).has_value());
    /// The same index inside an Array is refused the same way.
    EXPECT_EQ(decodeAndClassify("\x1e" + legacy_decimal512, BinaryTypeOrigin::Unknown), std::optional<bool>(true));

    /// Ordinary errors stay ordinary exceptions: an unknown type code, the complexity limit and truncated data.
    EXPECT_EQ(decodeAndClassify("\xff", BinaryTypeOrigin::Unknown), std::optional<bool>(false));
    EXPECT_EQ(decodeAndClassify("\x1e\x1e\x1e\x09", BinaryTypeOrigin::Unknown, /*max_complexity=*/ 2), std::optional<bool>(false));
    EXPECT_EQ(decodeAndClassify("\x1e", BinaryTypeOrigin::Unknown), std::optional<bool>(false));
}

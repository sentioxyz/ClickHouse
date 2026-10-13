#include <Functions/FunctionFactory.h>
#include <Functions/FunctionBinaryArithmetic.h>
#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <base/arithmeticOverflow.h>

namespace DB
{

namespace Setting
{
extern const SettingsDateTimeOverflowBehavior date_time_overflow_behavior;
}

FormatSettings::DateTimeOverflowBehavior getDateTimeOverflowBehavior(ContextPtr context)
{
    if (context)
        return context->getSettingsRef()[Setting::date_time_overflow_behavior].value;
    return default_date_time_overflow_behavior;
}

/// Overflow check of the checked `plus` with an `Int512` result (`Decimal512`): from the sign bits of x, y and the wrapped sum instead of
/// the range formula of `common::addOverflow`. The wrapped sum is computed in unsigned 512-bit arithmetic: the two's-complement bits
/// `common::addIgnoreOverflow` yields, without its signed `_BitInt(512)` addition, whose overflow is undefined. Overflow iff x and y have
/// the same sign and the wrapped sum has the other sign. x and y are copies, so a res that aliases an operand cannot change them before
/// their sign bits are read; `big(0)` is the most significant limb on either endianness. Covered by the stateless test
/// 10333_decimal512_plus_int512_overflow (scale 0, scale 1 and 0 to 154 with true overflow and fitting cancellation, Nullable).
static bool addOverflowInt512SignBits(Int512 x, Int512 y, Int512 & res)
{
    constexpr unsigned top_limb = Int512::_impl::big(0);
    const UInt64 x_top = x.items[top_limb];
    const UInt64 y_top = y.items[top_limb];
    res = static_cast<Int512>(static_cast<UInt512>(x) + static_cast<UInt512>(y));
    const UInt64 r_top = res.items[top_limb];
    return ((x_top ^ r_top) & (y_top ^ r_top) & (UInt64(1) << 63)) != 0;
}

template <typename A, typename B>
struct PlusImpl
{
    using ResultType = typename NumberTraits::ResultOfAdditionMultiplication<A, B>::Type;
    static const constexpr bool allow_fixed_string = false;
    static const constexpr bool allow_string_integer = false;
    static const constexpr bool is_commutative = true;

    template <typename Result = ResultType>
    static NO_SANITIZE_UNDEFINED Result apply(A a, B b)
    {
        /// Next everywhere, static_cast - so that there is no wrong result in expressions of the form Int64 c = UInt32(a) * Int32(-1).
        if constexpr (is_big_int_v<A> || is_big_int_v<B>)
        {
            using CastA = std::conditional_t<is_floating_point<B>, B, A>;
            using CastB = std::conditional_t<is_floating_point<A>, A, B>;

            return static_cast<Result>(static_cast<CastA>(a)) + static_cast<Result>(static_cast<CastB>(b));
        }
        else
            return static_cast<Result>(a) + static_cast<Result>(b);
    }

    /// Apply operation and check overflow. It's used for Deciamal operations. @returns true if overflowed, false otherwise.
    template <typename Result = ResultType>
    static bool apply(A a, B b, Result & c)
    {
        if constexpr (std::is_same_v<Result, Int512>)
            return addOverflowInt512SignBits(static_cast<Result>(a), b, c);
        else
            return common::addOverflow(static_cast<Result>(a), b, c);
    }

#if USE_EMBEDDED_COMPILER
    static constexpr bool compilable = true;

    static llvm::Value * compile(llvm::IRBuilder<> & b, llvm::Value * left, llvm::Value * right, bool)
    {
        return left->getType()->isIntegerTy() ? b.CreateAdd(left, right) : b.CreateFAdd(left, right);
    }
#endif
};

struct NamePlus { static constexpr auto name = "plus"; };
using FunctionPlus = BinaryArithmeticOverloadResolver<PlusImpl, NamePlus>;

REGISTER_FUNCTION(Plus)
{
    FunctionDocumentation::Description description = R"(
Calculates the sum of two values `x` and `y`. Alias: `x + y` (operator).
It is possible to add an integer and a date or date with time. The former
operation increments the number of days in the date, the latter operation
increments the number of seconds in the date with time.
It is also possible to add a date and a time. Adding a `Date` and a `Time`
produces a `DateTime`. Adding a `Date` and a `Time64`, or a `Date32` and
a `Time` or `Time64`, produces a `DateTime64`.
    )";
    FunctionDocumentation::Syntax syntax = "plus(x, y)";
    FunctionDocumentation::Argument argument1 = {"x", "Left hand operand."};
    FunctionDocumentation::Argument argument2 = {"y", "Right hand operand."};
    FunctionDocumentation::Arguments arguments = {argument1, argument2};
    FunctionDocumentation::ReturnedValue returned_value = {"Returns the sum of x and y"};
    FunctionDocumentation::Example example1 = {"Adding two numbers", "SELECT plus(5,5)", "10"};
    FunctionDocumentation::Example example2 = {"Adding an integer and a date", "SELECT plus(toDate('2025-01-01'),5)", "2025-01-06"};
    FunctionDocumentation::Example example3 = {"Adding a date and time", "SELECT toDate('2025-01-01') + CAST('14:30:25', 'Time')", "2025-01-01 14:30:25"};
    FunctionDocumentation::Examples examples = {example1, example2, example3};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Arithmetic;
    FunctionDocumentation::IntroducedIn introduced_in = {1, 1};
    FunctionDocumentation documentation = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionPlus>(documentation);
}

}

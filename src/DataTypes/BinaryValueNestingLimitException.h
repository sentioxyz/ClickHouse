#pragma once

#include <Common/Exception.h>


namespace DB
{

/// The value-nesting budget refusal, distinct from every other malformed-input error. It remains INCORRECT_DATA for binary value
/// normalization. RowBinary input catches exactly this C++ condition to preserve its TOO_DEEP_RECURSION payload contract. Keep the
/// outward DB::Exception name, message and code, and preserve the concrete type across clone/rethrow.
class BinaryValueNestingLimitException : public Exception
{
public:
    using Exception::Exception;

    BinaryValueNestingLimitException * clone() const override
    {
        return new BinaryValueNestingLimitException(*this);
    }

    void rethrow() const override
    {
        throw *this; /// NOLINT(bugprone-exception-copy-constructor-throws,cert-err60-cpp)
    }
};

}

#pragma once

#include <Common/Exception.h>


namespace DB
{

/// The refusal of a legacy binary type index of the Decimal512 fork (0x37, 0x38, 0x39) read with the Unknown origin: the index is ambiguous,
/// and only a declared producer origin (sentio_legacy_v0) reads it. Outwardly it is the exception it always was: the code INCORRECT_DATA,
/// the same message, and the name and class name of DB::Exception, so that every rendering of it stays the same. Only its C++ type is its
/// own, kept by clone and rethrow: a storage that loads its data can catch exactly this condition, which the registration of its stored
/// binary type origin resolves, apart from every other error with the same code (Storages/StorageSet.h).
class BinaryTypeOriginRequiredException : public Exception
{
public:
    using Exception::Exception;

    BinaryTypeOriginRequiredException * clone() const override { return new BinaryTypeOriginRequiredException(*this); }
    void rethrow() const override { throw *this; } /// NOLINT(bugprone-exception-copy-constructor-throws,cert-err60-cpp)
};

}

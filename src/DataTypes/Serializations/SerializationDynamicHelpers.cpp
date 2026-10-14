#include <DataTypes/Serializations/SerializationDynamicHelpers.h>
#include <DataTypes/DataTypeVariant.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/TypeNameIdentity.h>
#include <Columns/ColumnDynamic.h>
#include <Columns/ColumnVariant.h>
#include <Columns/ColumnsNumber.h>
#include <IO/ReadBufferFromMemory.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int INCORRECT_DATA;
}

namespace
{

/// Iterate over rows in Dynamic column and create an indexes column with indexes of each type in the flattened list.
template <typename IndexesColumn>
ColumnPtr createIndexes(
    const ColumnDynamic & dynamic_column,
    /// Map (discriminator -> index) for types that are stored separately as variants.
    const std::unordered_map<ColumnVariant::Discriminator, size_t> & discriminator_to_index,
    /// Map (type name -> index) for types that are stored together in shared variant.
    const std::unordered_map<String, size_t> & shared_variant_type_to_index,
    /// Index that should be used for Null values.
    size_t null_index)
{
    auto column = IndexesColumn::create();
    auto & data = column->getData();
    data.reserve(dynamic_column.size());
    const auto & variant_column = dynamic_column.getVariantColumn();
    const auto & local_discriminators = variant_column.getLocalDiscriminators();
    const auto & offsets = variant_column.getOffsets();
    auto shared_variant_discr = dynamic_column.getSharedVariantDiscriminator();
    const auto & shared_variant_column = dynamic_column.getSharedVariant();
    for (size_t i = 0; i != local_discriminators.size(); ++i)
    {
        auto global_discr = variant_column.globalDiscriminatorByLocal(local_discriminators[i]);
        if (global_discr == ColumnVariant::NULL_DISCRIMINATOR)
        {
            data.push_back(static_cast<IndexesColumn::ValueType>(null_index));
        }
        else if (global_discr == shared_variant_discr)
        {
            auto value = shared_variant_column.getDataAt(offsets[i]);
            ReadBufferFromMemory buf(value);
            auto type = decodeDataType(buf);
            data.push_back(static_cast<IndexesColumn::ValueType>(shared_variant_type_to_index.at(getTypeIdentityKey(type))));
        }
        else
        {
            data.push_back(static_cast<IndexesColumn::ValueType>(discriminator_to_index.at(global_discr)));
        }
    }

    return column;
}

}

FlattenedDynamicColumn flattenDynamicColumn(const ColumnDynamic & dynamic_column)
{
    const auto & variant_info = dynamic_column.getVariantInfo();
    const auto & variant_types = assert_cast<const DataTypeVariant &>(*variant_info.variant_type).getVariants();
    const auto & variant_column = dynamic_column.getVariantColumn();
    auto shared_variant_discr = dynamic_column.getSharedVariantDiscriminator();
    FlattenedDynamicColumn flattened_dynamic_column;
    /// Mapping from the discriminator of a variant to an index of this type in flattened list.
    std::unordered_map<ColumnVariant::Discriminator, size_t> discriminator_to_index;
    for (ColumnVariant::Discriminator i = 0; i != variant_types.size(); ++i)
    {
        /// SharedVariant will be processed later.
        if (i == shared_variant_discr)
            continue;

        discriminator_to_index[i] = flattened_dynamic_column.types.size();
        flattened_dynamic_column.types.push_back(variant_types[i]);
        flattened_dynamic_column.columns.push_back(variant_column.getVariantPtrByGlobalDiscriminator(i));
    }

    /// Collect all types from SharedVariant.
    std::unordered_map<String, std::pair<MutableColumnPtr, DataTypePtr>> shared_variant_columns;
    const auto & shared_variant_column = dynamic_column.getSharedVariant();
    FormatSettings format_settings;
    for (size_t i = 0; i != shared_variant_column.size(); ++i)
    {
        auto value = shared_variant_column.getDataAt(i);
        ReadBufferFromMemory buf(value);
        auto type = decodeDataType(buf);
        /// Types with the same name but different native classes stay apart (getTypeIdentityKey).
        auto type_key = getTypeIdentityKey(type);
        auto it = shared_variant_columns.find(type_key);
        /// If we see this type for the first time, add it to the map and create a column for it.
        if (it == shared_variant_columns.end())
            it = shared_variant_columns.emplace(type_key, std::make_pair(type->createColumn(), type)).first;

        /// Deserialize value into the corresponding column.
        type->getDefaultSerialization()->deserializeBinary(*it->second.first, buf, format_settings);
    }

    /// Append the deserialized shared-variant columns to the flattened list.
    flattened_dynamic_column.columns.reserve(flattened_dynamic_column.columns.size() + shared_variant_columns.size());
    flattened_dynamic_column.types.reserve(flattened_dynamic_column.types.size() + shared_variant_columns.size());
    /// Mapping from the type name from shared variant to an index of this type in flattened list.
    std::unordered_map<String, size_t> shared_variant_type_to_index;
    for (const auto & [type_key, column_with_type] : shared_variant_columns)
    {
        shared_variant_type_to_index[type_key] = flattened_dynamic_column.columns.size();
        flattened_dynamic_column.columns.push_back(column_with_type.first->getPtr());
        flattened_dynamic_column.types.push_back(column_with_type.second);
    }

    /// Now choose type for indexes column and create it.
    size_t num_indexes = flattened_dynamic_column.types.size() + 1; /// +1 for NULL index.
    flattened_dynamic_column.indexes_type = getSmallestIndexesType(num_indexes);
    switch (flattened_dynamic_column.indexes_type->getTypeId())
    {
        case TypeIndex::UInt8:
            flattened_dynamic_column.indexes_column = createIndexes<ColumnUInt8>(dynamic_column, discriminator_to_index, shared_variant_type_to_index, flattened_dynamic_column.types.size());
            break;
        case TypeIndex::UInt16:
            flattened_dynamic_column.indexes_column = createIndexes<ColumnUInt16>(dynamic_column, discriminator_to_index, shared_variant_type_to_index, flattened_dynamic_column.types.size());
            break;
        case TypeIndex::UInt32:
            flattened_dynamic_column.indexes_column = createIndexes<ColumnUInt32>(dynamic_column, discriminator_to_index, shared_variant_type_to_index, flattened_dynamic_column.types.size());
            break;
        case TypeIndex::UInt64:
            flattened_dynamic_column.indexes_column = createIndexes<ColumnUInt64>(dynamic_column, discriminator_to_index, shared_variant_type_to_index, flattened_dynamic_column.types.size());
            break;
        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected type as type of indices column type: {}", flattened_dynamic_column.indexes_type->getName());
    }

    return flattened_dynamic_column;
}

namespace
{

/// Fill usual Dynamic column with the data of flattened Dynamic column representation.
template <typename IndexesColumn>
void fillDynamicColumn(
    FlattenedDynamicColumn && flattened_column,
    ColumnDynamic & dynamic_column,
    /// Map (index -> discriminator) for types that will be stored as separate variants; all other types go to the shared variant.
    const std::unordered_map<size_t, ColumnVariant::Discriminator> & index_to_discriminator)
{
    const auto & indexes_data = assert_cast<const IndexesColumn &>(*flattened_column.indexes_column).getData();
    dynamic_column.reserve(dynamic_column.size() + indexes_data.size());
    auto & variant_column = dynamic_column.getVariantColumn();
    auto & local_discriminators = variant_column.getLocalDiscriminators();
    auto & offsets = variant_column.getOffsets();
    auto shared_variant_discr = dynamic_column.getSharedVariantDiscriminator();
    auto & shared_variant = dynamic_column.getSharedVariant();
    /// For NULL values we use index equal to the total number of types.
    size_t null_index = flattened_column.types.size();
    std::vector<size_t> flattened_columns_offsets(flattened_column.columns.size(), 0);
    for (size_t i = 0; i != indexes_data.size(); ++i)
    {
        auto index = indexes_data[i];
        if (index > null_index)
            throw Exception(
                ErrorCodes::INCORRECT_DATA,
                "Incorrect index {} in indexes column of flattened Dynamic column at row {}: "
                "the index should be in range [0, {}] (there are {} types, index {} is reserved for NULL values)",
                static_cast<UInt64>(index),
                i,
                null_index,
                flattened_column.types.size(),
                null_index);

        if (index == null_index)
        {
            local_discriminators.push_back(ColumnVariant::NULL_DISCRIMINATOR);
            offsets.emplace_back();
        }
        else if (auto discriminator_it = index_to_discriminator.find(index); discriminator_it != index_to_discriminator.end())
        {
            auto global_discr = discriminator_it->second;
            local_discriminators.push_back(variant_column.localDiscriminatorByGlobal(global_discr));
            /// If we see this type for the first time, replace the corresponding variant column with the data of this type.
            if (flattened_columns_offsets[index] == 0)
                variant_column.getVariantPtrByGlobalDiscriminator(global_discr) = IColumn::mutate(std::move(flattened_column.columns[index]));
            offsets.push_back(flattened_columns_offsets[index]);
            ++flattened_columns_offsets[index];
        }
        else
        {
            /// Insert value of this type into shared variant.
            local_discriminators.push_back(variant_column.localDiscriminatorByGlobal(shared_variant_discr));
            offsets.push_back(shared_variant.size());
            ColumnDynamic::serializeValueIntoSharedVariant(
                shared_variant,
                *flattened_column.columns[index],
                flattened_column.types[index],
                flattened_column.types[index]->getDefaultSerialization(),
                flattened_columns_offsets[index]);
            ++flattened_columns_offsets[index];
        }
    }
}

}

void unflattenDynamicColumn(FlattenedDynamicColumn && flattened_column, ColumnDynamic & dynamic_column)
{
    /// Iterate over types and try to add them as new variants into Dynamic column, deciding for every type on its own: a type goes to the
    /// shared variant if the limit is reached or if it cannot be a regular variant at all (a type that its name does not identify, see
    /// addNewVariant). Such a type does not send the types after it to the shared variant while the limit allows them as variants.
    std::vector<bool> is_regular_variant(flattened_column.types.size());
    for (size_t i = 0; i != flattened_column.types.size(); ++i)
        is_regular_variant[i] = dynamic_column.addNewVariant(flattened_column.types[i], flattened_column.types[i]->getName());

    /// Map (index -> discriminator) for types that were successfully added as variants. Adding a variant can change the discriminators of
    /// the others, so they are looked up after all types were added.
    std::unordered_map<size_t, ColumnVariant::Discriminator> index_to_discriminator;
    for (size_t i = 0; i != flattened_column.types.size(); ++i)
    {
        if (is_regular_variant[i])
            index_to_discriminator[i] = *dynamic_column.findVariantDiscriminator(flattened_column.types[i], flattened_column.types[i]->getName());
    }


    switch (flattened_column.indexes_type->getTypeId())
    {
        case TypeIndex::UInt8:
            fillDynamicColumn<ColumnUInt8>(std::move(flattened_column), dynamic_column, index_to_discriminator);
            break;
        case TypeIndex::UInt16:
            fillDynamicColumn<ColumnUInt16>(std::move(flattened_column), dynamic_column, index_to_discriminator);
            break;
        case TypeIndex::UInt32:
            fillDynamicColumn<ColumnUInt32>(std::move(flattened_column), dynamic_column, index_to_discriminator);
            break;
        case TypeIndex::UInt64:
            fillDynamicColumn<ColumnUInt64>(std::move(flattened_column), dynamic_column, index_to_discriminator);
            break;
        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected type of indexes in flattened Dynamic column: {}", flattened_column.indexes_type->getName());
    }
}

namespace
{

template <typename IndexesColumn>
std::vector<size_t> getLimitsImpl(const IColumn & indexes_column, size_t num_types)
{
    const auto & data = assert_cast<const IndexesColumn &>(indexes_column).getData();
    std::vector<size_t> limits(num_types);
    for (size_t i = 0; i != data.size(); ++i)
    {
        if (data[i] < num_types)
            ++limits[data[i]];
    }

    return limits;
}

}

std::vector<size_t> getLimitsForFlattenedDynamicColumn(const IColumn & indexes_column, size_t num_types)
{
    switch (indexes_column.getDataType())
    {
        case TypeIndex::UInt8:
            return getLimitsImpl<ColumnUInt8>(indexes_column, num_types);
        case TypeIndex::UInt16:
            return getLimitsImpl<ColumnUInt16>(indexes_column, num_types);
        case TypeIndex::UInt32:
            return getLimitsImpl<ColumnUInt32>(indexes_column, num_types);
        case TypeIndex::UInt64:
            return getLimitsImpl<ColumnUInt64>(indexes_column, num_types);
        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected column type of indexes in flattened Dynamic column: {}", indexes_column.getName());
    }
}

}

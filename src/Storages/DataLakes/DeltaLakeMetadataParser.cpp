#include <Storages/DataLakes/DeltaLakeMetadataParser.h>
#include <base/JSON.h>
#include "config.h"
#include <set>

#if USE_AWS_S3 && USE_PARQUET
#include <Storages/DataLakes/S3MetadataReader.h>
#include <Storages/StorageS3.h>
#include <parquet/file_reader.h>
#include <Processors/Formats/Impl/ArrowBufferedStreams.h>
#include <Processors/Formats/Impl/ParquetBlockInputFormat.h>
#include <Processors/Formats/Impl/ArrowColumnToCHColumn.h>
#include <Formats/FormatFactory.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnNullable.h>
#include <IO/ReadHelpers.h>
#include <boost/algorithm/string/case_conv.hpp>
#include <parquet/arrow/reader.h>
#include <ranges>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeUUID.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int NOT_IMPLEMENTED;
}

template <typename Configuration, typename MetadataReadHelper>
struct DeltaLakeMetadataParser<Configuration, MetadataReadHelper>::Impl
{
    /**
     * Useful links:
     *  - https://github.com/delta-io/delta/blob/master/PROTOCOL.md#data-files
     */

    /**
     * DeltaLake tables store metadata files and data files.
     * Metadata files are stored as JSON in a directory at the root of the table named _delta_log,
     * and together with checkpoints make up the log of all changes that have occurred to a table.
     *
     * Delta files are the unit of atomicity for a table,
     * and are named using the next available version number, zero-padded to 20 digits.
     * For example:
     *     ./_delta_log/00000000000000000000.json
     */
    static constexpr auto deltalake_metadata_directory = "_delta_log";
    static constexpr auto metadata_file_suffix = ".json";

    std::string withPadding(size_t version)
    {
        /// File names are zero-padded to 20 digits.
        static constexpr auto padding = 20;

        const auto version_str = toString(version);
        return std::string(padding - version_str.size(), '0') + version_str;
    }

    /**
     * A delta file, n.json, contains an atomic set of actions that should be applied to the
     * previous table state (n-1.json) in order to the construct nth snapshot of the table.
     * An action changes one aspect of the table's state, for example, adding or removing a file.
     * Note: it is not a valid json, but a list of json's, so we read it in a while cycle.
     */
    struct DeltaLakeMetadata
    {
        NamesAndTypesList schema;
        Strings data_files;
        DataLakePartitionColumns partition_columns;
    };
    DeltaLakeMetadata processMetadataFiles(const Configuration & configuration, ContextPtr context)
    {
        std::set<String> result_files;
        NamesAndTypesList current_schema;
        DataLakePartitionColumns current_partition_columns;
        const auto checkpoint_version = getCheckpointIfExists(result_files, configuration, context);

        if (checkpoint_version)
        {
            auto current_version = checkpoint_version;
            while (true)
            {
                const auto filename = withPadding(++current_version) + metadata_file_suffix;
                const auto file_path = fs::path(configuration.getPath()) / deltalake_metadata_directory / filename;

                if (!MetadataReadHelper::exists(file_path, configuration))
                    break;

                processMetadataFile(file_path, result_files, current_schema, current_partition_columns, configuration, context);
            }

            LOG_TRACE(
                log, "Processed metadata files from checkpoint {} to {}",
                checkpoint_version, current_version - 1);
        }
        else
        {
            const auto keys = MetadataReadHelper::listFiles(
                configuration, deltalake_metadata_directory, metadata_file_suffix);

            for (const String & key : keys)
                processMetadataFile(key, result_files, current_schema, current_partition_columns, configuration, context);
        }

        return DeltaLakeMetadata{current_schema, Strings(result_files.begin(), result_files.end()), current_partition_columns};
    }

    /**
     * Example of content of a single .json metadata file:
     * "
     *     {"commitInfo":{
     *         "timestamp":1679424650713,
     *         "operation":"WRITE",
     *         "operationMetrics":{"numFiles":"1","numOutputRows":"100","numOutputBytes":"2560"},
     *         ...}
     *     {"protocol":{"minReaderVersion":2,"minWriterVersion":5}}
     *     {"metaData":{
     *         "id":"bd11ad96-bc2c-40b0-be1f-6fdd90d04459",
     *         "format":{"provider":"parquet","options":{}},
     *         "schemaString":"{...}",
     *         "partitionColumns":[],
     *         "configuration":{...},
     *         "createdTime":1679424648640}}
     *     {"add":{
     *         "path":"part-00000-ecf8ed08-d04a-4a71-a5ec-57d8bb2ab4ee-c000.parquet",
     *         "partitionValues":{},
     *         "size":2560,
     *         "modificationTime":1679424649568,
     *         "dataChange":true,
     *         "stats":"{
     *             \"numRecords\":100,
     *             \"minValues\":{\"col-6c990940-59bb-4709-8f2e-17083a82c01a\":0},
     *             \"maxValues\":{\"col-6c990940-59bb-4709-8f2e-17083a82c01a\":99},
     *             \"nullCount\":{\"col-6c990940-59bb-4709-8f2e-17083a82c01a\":0,\"col-763cd7e2-7627-4d8e-9fb7-9e85d0c8845b\":0}}"}}
     * "
     */
    void processMetadataFile(
        const String & key,
        std::set<String> & result,
        NamesAndTypesList & file_schema,
        DataLakePartitionColumns & file_partition_columns,
        const Configuration & configuration,
        ContextPtr context)
    {
        auto buf = MetadataReadHelper::createReadBuffer(key, context, configuration);

        char c;
        while (!buf->eof())
        {
            /// May be some invalid characters before json.
            while (buf->peek(c) && c != '{')
                buf->ignore();

            if (buf->eof())
                break;

            String json_str;
            readJSONObjectPossiblyInvalid(json_str, *buf);

            if (json_str.empty())
                continue;

            Poco::JSON::Parser parser;
            Poco::Dynamic::Var json = parser.parse(json_str);
            Poco::JSON::Object::Ptr object = json.extract<Poco::JSON::Object::Ptr>();

            std::ostringstream oss;     // STYLE_CHECK_ALLOW_STD_STRING_STREAM
            object->stringify(oss);
            LOG_TEST(log, "Metadata: {}", oss.str());

            if (object->has("add"))
            {
                auto add_object = object->get("add").extract<Poco::JSON::Object::Ptr>();
                auto path = add_object->getValue<String>("path");
                result.insert(fs::path(configuration.getPath()) / path);

                auto filename = fs::path(path).filename().string();
                auto it = file_partition_columns.find(filename);
                if (it == file_partition_columns.end())
                {
                    auto partition_values = add_object->get("partitionValues").extract<Poco::JSON::Object::Ptr>();
                    if (partition_values->size())
                    {
                        auto & current_partition_columns = file_partition_columns[filename];
                        for (const auto & name : partition_values->getNames())
                        {
                            const auto value = partition_values->getValue<String>(name);
                            auto name_and_type = file_schema.tryGetByName(name);
                            if (!name_and_type)
                                throw Exception(ErrorCodes::LOGICAL_ERROR, "No such column in schema: {}", name);

                            auto field = getFieldValue(value, name_and_type->type);
                            current_partition_columns.emplace_back(*name_and_type, field);

                            LOG_TEST(log, "Partition {} value is {} (for {})", name, value, filename);
                        }
                    }
                }
            }
            else if (object->has("remove"))
            {
                auto path = object->get("remove").extract<Poco::JSON::Object::Ptr>()->getValue<String>("path");
                result.erase(fs::path(configuration.getPath()) / path);
            }
            if (object->has("metaData"))
            {
                const auto metadata_object = object->get("metaData").extract<Poco::JSON::Object::Ptr>();
                const auto schema_object = metadata_object->getValue<String>("schemaString");

                Poco::JSON::Parser p;
                Poco::Dynamic::Var fields_json = parser.parse(schema_object);
                Poco::JSON::Object::Ptr fields_object = fields_json.extract<Poco::JSON::Object::Ptr>();

                const auto fields = fields_object->get("fields").extract<Poco::JSON::Array::Ptr>();
                NamesAndTypesList current_schema;
                for (size_t i = 0; i < fields->size(); ++i)
                {
                    const auto field = fields->getObject(static_cast<UInt32>(i));
                    auto name = field->getValue<String>("name");
                    auto type = field->getValue<String>("type");
                    auto is_nullable = field->getValue<bool>("nullable");

                    std::string physical_name;
                    auto schema_metadata_object = field->get("metadata").extract<Poco::JSON::Object::Ptr>();
                    if (schema_metadata_object->has("delta.columnMapping.physicalName"))
                        physical_name = schema_metadata_object->getValue<String>("delta.columnMapping.physicalName");
                    else
                        physical_name = name;

                    LOG_TEST(log, "Found column: {}, type: {}, nullable: {}, physical name: {}",
                                name, type, is_nullable, physical_name);

                    current_schema.push_back({physical_name, getFieldType(field, "type", is_nullable)});
                }

                if (file_schema.empty())
                {
                    file_schema = current_schema;
                }
                else if (file_schema != current_schema)
                {
                    throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                                    "Reading from files with different schema is not possible "
                                    "({} is different from {})",
                                    file_schema.toString(), current_schema.toString());
                }
            }
        }
    }

    DataTypePtr getFieldType(const Poco::JSON::Object::Ptr & field, const String & type_key, bool is_nullable)
    {
        if (field->isObject(type_key))
            return getComplexTypeFromObject(field->getObject(type_key));

        auto type = field->get(type_key);
        if (type.isString())
        {
            const String & type_name = type.extract<String>();
            auto data_type = getSimpleTypeByName(type_name);
            return is_nullable ? makeNullable(data_type) : data_type;
        }

        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unexpected 'type' field: {}", type.toString());
    }

    Field getFieldValue(const String & value, DataTypePtr data_type)
    {
        DataTypePtr check_type;
        if (data_type->isNullable())
            check_type = static_cast<const DataTypeNullable *>(data_type.get())->getNestedType();
        else
            check_type = data_type;

        WhichDataType which(check_type->getTypeId());
        if (which.isStringOrFixedString())
            return value;
        else if (which.isInt8())
            return parse<Int8>(value);
        else if (which.isUInt8())
            return parse<UInt8>(value);
        else if (which.isInt16())
            return parse<Int16>(value);
        else if (which.isUInt16())
            return parse<UInt16>(value);
        else if (which.isInt32())
            return parse<Int32>(value);
        else if (which.isUInt32())
            return parse<UInt32>(value);
        else if (which.isInt64())
            return parse<Int64>(value);
        else if (which.isUInt64())
            return parse<UInt64>(value);
        else if (which.isFloat32())
            return parse<Float32>(value);
        else if (which.isFloat64())
            return parse<Float64>(value);
        else if (which.isDate())
            return UInt16{LocalDate{std::string(value)}.getDayNum()};
        else if (which.isDate32())
            return Int32{LocalDate{std::string(value)}.getExtenedDayNum()};
        else if (which.isDateTime64())
        {
            ReadBufferFromString in(value);
            DateTime64 time = 0;
            readDateTime64Text(time, 6, in, assert_cast<const DataTypeDateTime64 *>(data_type.get())->getTimeZone());
            return time;
        }

        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unsupported DeltaLake type for {}", check_type->getColumnType());
    }

    DataTypePtr getSimpleTypeByName(const String & type_name)
    {
        /// https://github.com/delta-io/delta/blob/master/PROTOCOL.md#primitive-types

        if (type_name == "string" || type_name == "binary")
            return std::make_shared<DataTypeString>();
        if (type_name == "long")
            return std::make_shared<DataTypeInt64>();
        if (type_name == "integer")
            return std::make_shared<DataTypeInt32>();
        if (type_name == "short")
            return std::make_shared<DataTypeInt16>();
        if (type_name == "byte")
            return std::make_shared<DataTypeInt8>();
        if (type_name == "float")
            return std::make_shared<DataTypeFloat32>();
        if (type_name == "double")
            return std::make_shared<DataTypeFloat64>();
        if (type_name == "boolean")
            return DataTypeFactory::instance().get("Bool");
        if (type_name == "date")
            return std::make_shared<DataTypeDate32>();
        if (type_name == "timestamp")
            return std::make_shared<DataTypeDateTime64>(6);
        if (type_name.starts_with("decimal(") && type_name.ends_with(')'))
        {
            ReadBufferFromString buf(std::string_view(type_name.begin() + 8, type_name.end() - 1));
            size_t precision;
            size_t scale;
            readIntText(precision, buf);
            skipWhitespaceIfAny(buf);
            assertChar(',', buf);
            skipWhitespaceIfAny(buf);
            tryReadIntText(scale, buf);
            return createDecimal<DataTypeDecimal>(precision, scale);
        }

        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unsupported DeltaLake type: {}", type_name);
    }

    DataTypePtr getComplexTypeFromObject(const Poco::JSON::Object::Ptr & type)
    {
        String type_name = type->getValue<String>("type");

        if (type_name == "struct")
        {
            DataTypes element_types;
            Names element_names;
            auto fields = type->get("fields").extract<Poco::JSON::Array::Ptr>();
            element_types.reserve(fields->size());
            element_names.reserve(fields->size());
            for (size_t i = 0; i != fields->size(); ++i)
            {
                auto field = fields->getObject(static_cast<Int32>(i));
                element_names.push_back(field->getValue<String>("name"));
                auto required = field->getValue<bool>("required");
                element_types.push_back(getFieldType(field, "type", required));
            }

            return std::make_shared<DataTypeTuple>(element_types, element_names);
        }

        if (type_name == "array")
        {
            bool is_nullable = type->getValue<bool>("containsNull");
            auto element_type = getFieldType(type, "elementType", is_nullable);
            return std::make_shared<DataTypeArray>(element_type);
        }

        if (type_name == "map")
        {
            bool is_nullable = type->getValue<bool>("containsNull");
            auto key_type = getFieldType(type, "keyType", /* is_nullable */false);
            auto value_type = getFieldType(type, "valueType", is_nullable);
            return std::make_shared<DataTypeMap>(key_type, value_type);
        }

        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unsupported DeltaLake type: {}", type_name);
    }

    /**
     * Checkpoints in delta-lake are created each 10 commits by default.
     * Latest checkpoint is written in _last_checkpoint file: _delta_log/_last_checkpoint
     *
     * _last_checkpoint contains the following:
     * {"version":20,
     *  "size":23,
     *  "sizeInBytes":14057,
     *  "numOfAddFiles":21,
     *  "checkpointSchema":{...}}
     *
     *  We need to get "version", which is the version of the checkpoint we need to read.
     */
    size_t readLastCheckpointIfExists(const Configuration & configuration, ContextPtr context)
    {
        const auto last_checkpoint_file = fs::path(configuration.getPath()) / deltalake_metadata_directory / "_last_checkpoint";
        if (!MetadataReadHelper::exists(last_checkpoint_file, configuration))
            return 0;

        String json_str;
        auto buf = MetadataReadHelper::createReadBuffer(last_checkpoint_file, context, configuration);
        readJSONObjectPossiblyInvalid(json_str, *buf);

        const JSON json(json_str);
        const auto version = json["version"].getUInt();

        LOG_TRACE(log, "Last checkpoint file version: {}", version);
        return version;
    }

    /**
     *  The format of the checkpoint file name can take one of two forms:
     *  1. A single checkpoint file for version n of the table will be named n.checkpoint.parquet.
     *     For example:
     *         00000000000000000010.checkpoint.parquet
     *  2. A multi-part checkpoint for version n can be fragmented into p files. Fragment o of p is
     *     named n.checkpoint.o.p.parquet. For example:
     *         00000000000000000010.checkpoint.0000000001.0000000003.parquet
     *         00000000000000000010.checkpoint.0000000002.0000000003.parquet
     *         00000000000000000010.checkpoint.0000000003.0000000003.parquet
     *  TODO: Only (1) is supported, need to support (2).
     *
     *  Such checkpoint files parquet contain data with the following contents:
     *
     *  Row 1:
     *  ──────
     *  txn:      (NULL,NULL,NULL)
     *  add:      ('part-00000-1e9cd0c1-57b5-43b4-9ed8-39854287b83a-c000.parquet',{},1070,1680614340485,false,{},'{"numRecords":1,"minValues":{"col-360dade5-6d0e-4831-8467-a25d64695975":13,"col-e27b0253-569a-4fe1-8f02-f3342c54d08b":"14"},"maxValues":{"col-360dade5-6d0e-4831-8467-a25d64695975":13,"col-e27b0253-569a-4fe1-8f02-f3342c54d08b":"14"},"nullCount":{"col-360dade5-6d0e-4831-8467-a25d64695975":0,"col-e27b0253-569a-4fe1-8f02-f3342c54d08b":0}}')
     *  remove:   (NULL,NULL,NULL,NULL,{},NULL,{})
     *  metaData: (NULL,NULL,NULL,(NULL,{}),NULL,[],{},NULL)
     *  protocol: (NULL,NULL)
     *
     *  Row 2:
     *  ──────
     *  txn:      (NULL,NULL,NULL)
     *  add:      ('part-00000-8887e898-91dd-4951-a367-48f7eb7bd5fd-c000.parquet',{},1063,1680614318485,false,{},'{"numRecords":1,"minValues":{"col-360dade5-6d0e-4831-8467-a25d64695975":2,"col-e27b0253-569a-4fe1-8f02-f3342c54d08b":"3"},"maxValues":{"col-360dade5-6d0e-4831-8467-a25d64695975":2,"col-e27b0253-569a-4fe1-8f02-f3342c54d08b":"3"},"nullCount":{"col-360dade5-6d0e-4831-8467-a25d64695975":0,"col-e27b0253-569a-4fe1-8f02-f3342c54d08b":0}}')
     *  remove:   (NULL,NULL,NULL,NULL,{},NULL,{})
     *  metaData: (NULL,NULL,NULL,(NULL,{}),NULL,[],{},NULL)
     *  protocol: (NULL,NULL)
     *
     * We need to check only `add` column, `remove` column does not have intersections with `add` column.
     *  ...
     */
    #define THROW_ARROW_NOT_OK(status)                                    \
        do                                                                \
        {                                                                 \
            if (const ::arrow::Status & _s = (status); !_s.ok())          \
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Arrow error: {}", _s.ToString()); \
        } while (false)

    size_t getCheckpointIfExists(std::set<String> & result, const Configuration & configuration, ContextPtr context)
    {
        const auto version = readLastCheckpointIfExists(configuration, context);
        if (!version)
            return 0;

        const auto checkpoint_filename = withPadding(version) + ".checkpoint.parquet";
        const auto checkpoint_path = fs::path(configuration.getPath()) / deltalake_metadata_directory / checkpoint_filename;

        LOG_TRACE(log, "Using checkpoint file: {}", checkpoint_path.string());

        auto buf = MetadataReadHelper::createReadBuffer(checkpoint_path, context, configuration);
        auto format_settings = getFormatSettings(context);

        /// Force nullable, because this parquet file for some reason does not have nullable
        /// in parquet file metadata while the type are in fact nullable.
        format_settings.schema_inference_make_columns_nullable = true;
        auto columns = ParquetSchemaReader(*buf, format_settings).readSchema();

        /// Read only columns that we need.
        columns.filterColumns(NameSet{"add", "remove"});
        Block header;
        for (const auto & column : columns)
            header.insert({column.type->createColumn(), column.type, column.name});

        std::atomic<int> is_stopped{0};
        auto arrow_file = asArrowFile(*buf, format_settings, is_stopped, "Parquet", PARQUET_MAGIC_BYTES);

        std::unique_ptr<parquet::arrow::FileReader> reader;
        THROW_ARROW_NOT_OK(
            parquet::arrow::OpenFile(
                asArrowFile(*buf, format_settings, is_stopped, "Parquet", PARQUET_MAGIC_BYTES),
                arrow::default_memory_pool(),
                &reader));

        std::shared_ptr<arrow::Schema> file_schema;
        THROW_ARROW_NOT_OK(reader->GetSchema(&file_schema));

        ArrowColumnToCHColumn column_reader(
            header, "Parquet",
            format_settings.parquet.allow_missing_columns,
            /* null_as_default */true,
            format_settings.date_time_overflow_behavior,
            /* case_insensitive_column_matching */false);

        std::shared_ptr<arrow::Table> table;
        THROW_ARROW_NOT_OK(reader->ReadTable(&table));

        Chunk res = column_reader.arrowTableToCHChunk(table, reader->parquet_reader()->metadata()->num_rows());
        const auto & res_columns = res.getColumns();

        if (res_columns.size() != 2)
        {
            throw Exception(
                ErrorCodes::INCORRECT_DATA,
                "Unexpected number of columns: {} (having: {}, expected: {})",
                res_columns.size(), res.dumpStructure(), header.dumpStructure());
        }

        const auto * tuple_column = assert_cast<const ColumnTuple *>(res_columns[0].get());
        const auto & nullable_column = assert_cast<const ColumnNullable &>(tuple_column->getColumn(0));
        const auto & path_column = assert_cast<const ColumnString &>(nullable_column.getNestedColumn());
        for (size_t i = 0; i < path_column.size(); ++i)
        {
            const auto filename = String(path_column.getDataAt(i));
            if (filename.empty())
                continue;
            LOG_TEST(log, "Adding {}", filename);
            const auto [_, inserted] = result.insert(fs::path(configuration.getPath()) / filename);
            if (!inserted)
                throw Exception(ErrorCodes::INCORRECT_DATA, "File already exists {}", filename);
        }

        return version;
    }

    LoggerPtr log = getLogger("DeltaLakeMetadataParser");
};


template <typename Configuration, typename MetadataReadHelper>
DeltaLakeMetadataParser<Configuration, MetadataReadHelper>::DeltaLakeMetadataParser(const Configuration & configuration, ContextPtr context)
    : impl(std::make_unique<Impl>())
{
    auto result = impl->processMetadataFiles(configuration, context);
    data_files = result.data_files;
    schema = result.schema;
    partition_columns = result.partition_columns;

    LOG_TRACE(impl->log, "Found {} data files, {} partition files, schema: {}",
             data_files.size(), partition_columns.size(), schema.toString());
}

template DeltaLakeMetadataParser<StorageS3::Configuration, S3DataLakeMetadataReadHelper>::DeltaLakeMetadataParser(const StorageS3::Configuration & configuration, ContextPtr context);
}

#endif

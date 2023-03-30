#include "config.h"

#if USE_AWS_S3 && USE_AVRO

#include <Common/logger_useful.h>

#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>
#include <Columns/IColumn.h>
#include <Storages/DataLakes/IcebergMetadataParser.h>
#include <Storages/DataLakes/S3MetadataReader.h>
#include <Storages/StorageS3.h>
#include <Processors/Formats/Impl/AvroRowInputFormat.h>
#include <Formats/FormatFactory.h>

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int ILLEGAL_COLUMN;
}

template <typename Configuration, typename MetadataReadHelper>
struct IcebergMetadataParser<Configuration, MetadataReadHelper>::Impl
{
    /**
     * Useful links:
     * - https://iceberg.apache.org/spec/
     */

    /**
     * Iceberg has two format versions, currently we support only format V1.
     *
     * Unlike DeltaLake, Iceberg has several metadata layers: `table metadata`, `manifest list` and `manifest_files`.
     * Metadata file - json file.
     * Manifest list – a file that lists manifest files; one per snapshot.
     * Manifest file – a file that lists data or delete files; a subset of a snapshot.
     * All changes to table state create a new metadata file and replace the old metadata with an atomic swap.
     */

    static constexpr auto metadata_directory = "metadata";

    /**
     * Each version of table metadata is stored in a `metadata` directory and
     * has format: v<V>.metadata.json, where V - metadata version.
     */
    String getMetadataFile(const Configuration & configuration)
    {
        static constexpr auto metadata_file_suffix = ".metadata.json";

        const auto metadata_files = MetadataReadHelper::listFiles(configuration, metadata_directory, metadata_file_suffix);
        if (metadata_files.empty())
        {
            throw Exception(
                ErrorCodes::FILE_DOESNT_EXIST,
                "The metadata file for Iceberg table with path {} doesn't exist",
                configuration.url.key);
        }

        /// Get the latest version of metadata file: v<V>.metadata.json
        return *std::max_element(metadata_files.begin(), metadata_files.end());
    }

    /**
     * In order to find out which data files to read, we need to find the `manifest list`
     * which corresponds to the latest snapshot. We find it by checking a list of snapshots
     * in metadata's "snapshots" section.
     *
     * Example of metadata.json file.
     *
     * {
     *     "format-version" : 1,
     *     "table-uuid" : "ca2965ad-aae2-4813-8cf7-2c394e0c10f5",
     *     "location" : "/iceberg_data/default/test_single_iceberg_file",
     *     "last-updated-ms" : 1680206743150,
     *     "last-column-id" : 2,
     *     "schema" : { "type" : "struct", "schema-id" : 0, "fields" : [ {<field1_info>}, {<field2_info>}, ... ] },
     *     "current-schema-id" : 0,
     *     "schemas" : [ ],
     *     ...
     *     "current-snapshot-id" : 2819310504515118887,
     *     "refs" : { "main" : { "snapshot-id" : 2819310504515118887, "type" : "branch" } },
     *     "snapshots" : [ {
     *       "snapshot-id" : 2819310504515118887,
     *       "timestamp-ms" : 1680206743150,
     *       "summary" : {
     *         "operation" : "append", "spark.app.id" : "local-1680206733239",
     *         "added-data-files" : "1", "added-records" : "100",
     *         "added-files-size" : "1070", "changed-partition-count" : "1",
     *         "total-records" : "100", "total-files-size" : "1070", "total-data-files" : "1", "total-delete-files" : "0",
     *         "total-position-deletes" : "0", "total-equality-deletes" : "0"
     *       },
     *       "manifest-list" : "/iceberg_data/default/test_single_iceberg_file/metadata/snap-2819310504515118887-1-c87bfec7-d36c-4075-ad04-600b6b0f2020.avro",
     *       "schema-id" : 0
     *     } ],
     *     "statistics" : [ ],
     *     "snapshot-log" : [ ... ],
     *     "metadata-log" : [ ]
     * }
     */
    String getManifestListFromMetadata(const Configuration & configuration, ContextPtr context)
    {
        const auto metadata_file_path = getMetadataFile(configuration);
        auto buf = MetadataReadHelper::createReadBuffer(metadata_file_path, context, configuration);
        String json_str;
        readJSONObjectPossiblyInvalid(json_str, *buf);

        /// Looks like base/base/JSON.h can not parse this json file
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var json = parser.parse(json_str);
        Poco::JSON::Object::Ptr object = json.extract<Poco::JSON::Object::Ptr>();

        auto current_snapshot_id = object->getValue<Int64>("current-snapshot-id");
        auto snapshots = object->get("snapshots").extract<Poco::JSON::Array::Ptr>();

        for (size_t i = 0; i < snapshots->size(); ++i)
        {
            auto snapshot = snapshots->getObject(static_cast<UInt32>(i));
            if (snapshot->getValue<Int64>("snapshot-id") == current_snapshot_id)
            {
                auto path = snapshot->getValue<String>("manifest-list");
                return std::filesystem::path(configuration.url.key) / metadata_directory / std::filesystem::path(path).filename();
            }
        }

        return {};
    }

    /**
     * Manifest list has Avro as default format (and currently we support only Avro).
     * Manifest list file format of manifest list is: snap-2819310504515118887-1-c87bfec7-d36c-4075-ad04-600b6b0f2020.avro
     *
     * `manifest list` has the following contents:
     * ┌─manifest_path────────────────────────────────────────────────────────────────────────────────────────┬─manifest_length─┬─partition_spec_id─┬───added_snapshot_id─┬─added_data_files_count─┬─existing_data_files_count─┬─deleted_data_files_count─┬─partitions─┬─added_rows_count─┬─existing_rows_count─┬─deleted_rows_count─┐
     * │ /iceberg_data/default/test_single_iceberg_file/metadata/c87bfec7-d36c-4075-ad04-600b6b0f2020-m0.avro │            5813 │                 0 │ 2819310504515118887 │                      1 │                         0 │                        0 │ []         │              100 │                   0 │                  0 │
     * └──────────────────────────────────────────────────────────────────────────────────────────────────────┴─────────────────┴───────────────────┴─────────────────────┴────────────────────────┴───────────────────────────┴──────────────────────────┴────────────┴──────────────────┴─────────────────────┴────────────────────┘
     */

    Strings getManifestFiles(const String & manifest_list, const Configuration & configuration, ContextPtr context)
    {
        static constexpr auto manifest_path = "manifest_path";

        auto buf = MetadataReadHelper::createReadBuffer(manifest_list, context, configuration);
        auto file_reader = std::make_unique<avro::DataFileReaderBase>(std::make_unique<AvroInputStreamReadBufferAdapter>(*buf));

        auto data_type = AvroSchemaReader::avroNodeToDataType(file_reader->dataSchema().root()->leafAt(0));
        auto columns = parseAvro(file_reader, data_type, manifest_path, getFormatSettings(context));
        auto & col = columns.at(0);

        std::vector<String> res;
        if (col->getDataType() == TypeIndex::String)
        {
            const auto * col_str = typeid_cast<ColumnString *>(col.get());
            for (size_t i = 0; i < col_str->size(); ++i)
            {
                const auto file_path = col_str->getDataAt(i).toView();
                const auto filename = std::filesystem::path(file_path).filename();
                res.emplace_back(std::filesystem::path(configuration.url.key) / metadata_directory / filename);
            }

            return res;
        }

        throw Exception(
            ErrorCodes::ILLEGAL_COLUMN,
            "The parsed column from Avro file of `manifest_path` field should be String type, got {}",
            col->getFamilyName());
    }

    MutableColumns parseAvro(
        const std::unique_ptr<avro::DataFileReaderBase> & file_reader,
        const DataTypePtr & data_type,
        const String & field_name,
        const FormatSettings & settings)
    {
        auto deserializer = std::make_unique<AvroDeserializer>(
            Block{{data_type->createColumn(), data_type, field_name}}, file_reader->dataSchema(), true, true, settings);
        file_reader->init();
        MutableColumns columns;
        columns.emplace_back(data_type->createColumn());

        RowReadExtension ext;
        while (file_reader->hasMore())
        {
            file_reader->decr();
            deserializer->deserializeRow(columns, file_reader->decoder(), ext);
        }
        return columns;
    }

    /**
     * Manifest file has the following format: '/iceberg_data/default/test_single_iceberg_file/metadata/c87bfec7-d36c-4075-ad04-600b6b0f2020-m0.avro'
     *
     * `manifest list` has the following contents:
     * ┌─status─┬─────────snapshot_id─┬─data_file───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
     * │      1 │ 2819310504515118887 │ ('/iceberg_data/default/test_single_iceberg_file/data/00000-1-3edca534-15a0-4f74-8a28-4733e0bf1270-00001.parquet','PARQUET',(),100,1070,67108864,[(1,233),(2,210)],[(1,100),(2,100)],[(1,0),(2,0)],[],[(1,'\0'),(2,'0')],[(1,'c'),(2,'99')],NULL,[4],0) │
     * └────────┴─────────────────────┴─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
     */

    Strings getFilesForRead(const std::vector<String> & manifest_files, const Configuration & configuration, ContextPtr context)
    {
        Strings keys;
        for (const auto & manifest_file : manifest_files)
        {
            auto buffer = MetadataReadHelper::createReadBuffer(manifest_file, context, configuration);

            auto file_reader = std::make_unique<avro::DataFileReaderBase>(std::make_unique<AvroInputStreamReadBufferAdapter>(*buffer));

            static constexpr auto manifest_path = "data_file";

            /// The data_file filed at the 3rd position of the manifest file:
            /// {'status': xx, 'snapshot_id': xx, 'data_file': {'file_path': 'xxx', ...}, ...}
            /// and it's also a nested record, so its result type is a nested Tuple
            auto data_type = AvroSchemaReader::avroNodeToDataType(file_reader->dataSchema().root()->leafAt(2));
            auto columns = parseAvro(file_reader, data_type, manifest_path, getFormatSettings(context));
            auto & col = columns.at(0);

            if (col->getDataType() == TypeIndex::Tuple)
            {
                auto * col_tuple = typeid_cast<ColumnTuple *>(col.get());
                auto & col_str = col_tuple->getColumnPtr(0);
                if (col_str->getDataType() == TypeIndex::String)
                {
                    const auto * str_col = typeid_cast<const ColumnString *>(col_str.get());
                    size_t col_size = str_col->size();
                    for (size_t i = 0; i < col_size; ++i)
                    {
                        auto file_path = str_col->getDataAt(i).toView();
                        /// We just obtain the partition/file name
                        std::filesystem::path path(file_path);
                        keys.emplace_back(path.parent_path().filename() / path.filename());
                    }
                }
                else
                {
                    throw Exception(
                        ErrorCodes::ILLEGAL_COLUMN,
                        "The parsed column from Avro file of `file_path` field should be String type, got {}",
                        col_str->getFamilyName());
                }
            }
            else
            {
                throw Exception(
                    ErrorCodes::ILLEGAL_COLUMN,
                    "The parsed column from Avro file of `data_file` field should be Tuple type, got {}",
                    col->getFamilyName());
            }
        }

        return keys;
    }
};


template <typename Configuration, typename MetadataReadHelper>
IcebergMetadataParser<Configuration, MetadataReadHelper>::IcebergMetadataParser() : impl(std::make_unique<Impl>())
{
}

template <typename Configuration, typename MetadataReadHelper>
Strings IcebergMetadataParser<Configuration, MetadataReadHelper>::getFiles(const Configuration & configuration, ContextPtr context)
{
    auto manifest_list = impl->getManifestListFromMetadata(configuration, context);

    /// When table first created and does not have any data
    if (manifest_list.empty())
        return {};

    auto manifest_files = impl->getManifestFiles(manifest_list, configuration, context);
    return impl->getFilesForRead(manifest_files, configuration, context);
}


template IcebergMetadataParser<StorageS3::Configuration, S3DataLakeMetadataReadHelper>::IcebergMetadataParser();
template Strings IcebergMetadataParser<StorageS3::Configuration, S3DataLakeMetadataReadHelper>::getFiles(const StorageS3::Configuration & configuration, ContextPtr);

}

#endif

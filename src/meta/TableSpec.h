/*
 * Copyright 2017-present Shawn Cao
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <unordered_map>

#include "meta/Table.h"
#include "type/Serde.h"
#include "type/Type.h"

/**
 * Define nebula table and system metadata 
 * which manages what data segments are loaded in memory for each table
 * This meta data can persist and sync with external DB system such as MYSQL or RocksDB
 * (A KV store is necessary for Nebula to manage all metadata)
 * 
 * (Also - Is this responsibility of zookeeper?)
 */
namespace nebula {
namespace meta {

// define data source
enum class DataSource {
  Custom,
  S3,
  LOCAL,
  KAFKA,
  GSHEET
};

struct DataSourceUtils {
  static bool isFileSystem(const DataSource& ds) {
    return ds == DataSource::S3 || ds == DataSource::LOCAL;
  }

  static const std::string& getProtocol(const DataSource& ds) {
    static const std::string NONE = "";
    static const nebula::common::unordered_map<DataSource, std::string> SOURCE_PROTO = {
      { DataSource::S3, "s3" },
      { DataSource::LOCAL, "local" }
    };

    auto p = SOURCE_PROTO.find(ds);
    if (p != SOURCE_PROTO.end()) {
      return p->second;
    }

    return NONE;
  }
};

// type of time source to fill time column
enum class TimeType {
  // fixed value
  STATIC,
  // using current timestamp when loading
  CURRENT,
  // time is from a given column
  COLUMN,
  // system defined macro named by pattern
  MACRO,
  // system will provide depending on sub-system behavior
  // such as, Kafka will fill message timestamp for it
  PROVIDED
};

// type of macros accepted in table spec
enum class PatternMacro {
  // Daily partition /dt=?
  DATE,
  // hourly partition name /dt=?/hr=?
  HOUR,
  // minute partition name /dt=?/hr=?/mi=?
  MINUTE,
  // use second level directory name /dt=?/hr=?/mi=?/se=?
  SECOND,
  // use directory name in unix timestamp /ts=?
  TIMESTAMP,
  // placeholder for not accepted marcos
  INVALID,
};

struct TimeSpec {
  TimeType type;
  // unix time value if provided
  size_t unixTimeValue;
  // column name for given
  std::string colName;
  // time pattern to parse value out
  // if pattern not given which implies it is a string column
  // the column will be treated as integer of unix time value
  std::string pattern;
};

// serde info for some data format, such as thrift
struct KafkaSerde {
  // kafka topic retention of seconds
  uint64_t retention = 0;

  // size of each ingestion batch
  uint64_t size = 0;

  // protocol - thrift has binary, or compact protocol
  // json may have bson variant
  std::string protocol;

  // column map from column name to field ID
  // which should be defined by thrift schema
  nebula::common::unordered_map<std::string, uint32_t> cmap;
};

// TODO(cao): use nebula::common::unordered_map if it supports msgpack serde
// key-value settings in both string types
using Settings = std::unordered_map<std::string, std::string>;

struct TableSpec {
  // table name
  std::string name;
  // max size in MB resident in memory
  size_t max_mb;
  // max time span in hour resident in memory
  size_t max_hr;
  // table schema
  std::string schema;
  // data source to load from
  DataSource source;
  // loader to decide how to load data in
  std::string loader;
  // source location uri
  std::string location;
  // backup location uri
  std::string backup;
  // data format
  std::string format;
  // Serde of the data
  KafkaSerde serde;
  // column properties
  ColumnProps columnProps;
  // time spec to generate time value
  TimeSpec timeSpec;
  // access spec
  AccessSpec accessSpec;
  // bucket info
  BucketInfo bucketInfo;
  // settings spec just get list of key-values
  Settings settings;

  TableSpec(std::string n, size_t mm, size_t mh, std::string s,
            DataSource ds, std::string lo, std::string loc, std::string bak,
            std::string f, KafkaSerde sd, ColumnProps cp, TimeSpec ts,
            AccessSpec as, BucketInfo bi, Settings st)
    : name{ std::move(n) },
      max_mb{ mm },
      max_hr{ mh },
      schema{ std::move(s) },
      source{ ds },
      loader{ std::move(lo) },
      location{ std::move(loc) },
      backup{ std::move(bak) },
      format{ std::move(f) },
      serde{ std::move(sd) },
      columnProps{ std::move(cp) },
      timeSpec{ std::move(ts) },
      accessSpec{ std::move(as) },
      bucketInfo{ std::move(bi) },
      settings{ std::move(st) } {}

  inline std::string toString() const {
    // table name @ location - format: time
    return fmt::format("{0}@{1}-{2}: {3}", name, location, format, timeSpec.unixTimeValue);
  }

  // generate table pointer
  std::shared_ptr<Table> to() const {
    // raw schema to manipulate on
    auto schemaPtr = nebula::type::TypeSerializer::from(schema);

    // we need a time column for any input data source
    schemaPtr->addChild(nebula::type::LongType::createTree(Table::TIME_COLUMN));

    // if time column is provided by input data, we will remove it for final schema
    if (timeSpec.type == TimeType::COLUMN) {
      schemaPtr->remove(timeSpec.colName);
    }

    // build up a new table from this spec
    return std::make_shared<Table>(name, schemaPtr, columnProps, accessSpec);
  }
};

// define table spec pointer
using TableSpecPtr = std::shared_ptr<TableSpec>;

// Current hash and equal are based on table name only
// There should not be duplicate table names in the system
struct TableSpecHash {
public:
  size_t operator()(const TableSpecPtr& ts) const {
    return nebula::common::Hasher::hashString(ts->name);
  }
};

struct TableSpecEqual {
public:
  bool operator()(const TableSpecPtr& ts1, const TableSpecPtr& ts2) const {
    return ts1->name == ts2->name;
  }
};

using TableSpecSet = nebula::common::unordered_set<TableSpecPtr, TableSpecHash, TableSpecEqual>;

constexpr auto HOUR_MINUTES = 60;
constexpr auto MINUTE_SECONDS = 60;
constexpr auto DAY_HOURS = 24;
constexpr auto HOUR_SECONDS = HOUR_MINUTES * MINUTE_SECONDS;
constexpr auto DAY_SECONDS = HOUR_SECONDS * DAY_HOURS;

const nebula::common::unordered_map<nebula::meta::PatternMacro, std::string> patternYMLStr{
  { nebula::meta::PatternMacro::DATE, "DATE" },
  { nebula::meta::PatternMacro::HOUR, "HOUR" },
  { nebula::meta::PatternMacro::MINUTE, "MINUTE" },
  { nebula::meta::PatternMacro::SECOND, "SECOND" },
  { nebula::meta::PatternMacro::TIMESTAMP, "TIMESTAMP" }
};

const nebula::common::unordered_map<nebula::meta::PatternMacro, nebula::meta::PatternMacro> childPattern{
  { nebula::meta::PatternMacro::DATE, nebula::meta::PatternMacro::HOUR },
  { nebula::meta::PatternMacro::HOUR, nebula::meta::PatternMacro::MINUTE },
  { nebula::meta::PatternMacro::MINUTE, nebula::meta::PatternMacro::SECOND }
};

const nebula::common::unordered_map<nebula::meta::PatternMacro, int> unitInSeconds{
  { nebula::meta::PatternMacro::DATE, DAY_SECONDS },
  { nebula::meta::PatternMacro::HOUR, HOUR_SECONDS },
  { nebula::meta::PatternMacro::MINUTE, MINUTE_SECONDS }
};

const nebula::common::unordered_map<nebula::meta::PatternMacro, int> childSize{
  { nebula::meta::PatternMacro::DATE, DAY_HOURS },
  { nebula::meta::PatternMacro::HOUR, HOUR_MINUTES },
  { nebula::meta::PatternMacro::MINUTE, MINUTE_SECONDS }
};

// check if pattern string type
inline nebula::meta::PatternMacro extractPatternMacro(std::string pattern) {
  const auto tsMacroFound = pattern.find(patternYMLStr.at(PatternMacro::TIMESTAMP)) != std::string::npos;
  const auto dateMacroFound = pattern.find(patternYMLStr.at(PatternMacro::DATE)) != std::string::npos;
  const auto hourMacroFound = pattern.find(patternYMLStr.at(PatternMacro::HOUR)) != std::string::npos;
  const auto minuteMacroFound = pattern.find(patternYMLStr.at(PatternMacro::MINUTE)) != std::string::npos;
  const auto secondMacroFound = pattern.find(patternYMLStr.at(PatternMacro::SECOND)) != std::string::npos;

  if (secondMacroFound && minuteMacroFound && hourMacroFound && dateMacroFound) {
    return PatternMacro::SECOND;
  } else if (minuteMacroFound && hourMacroFound && dateMacroFound) {
    return PatternMacro::MINUTE;
  } else if (hourMacroFound && dateMacroFound && !secondMacroFound) {
    return PatternMacro::HOUR;
  } else if (dateMacroFound && !minuteMacroFound && !secondMacroFound) {
    return PatternMacro::DATE;
  }

  if (tsMacroFound && !secondMacroFound && !minuteMacroFound && !hourMacroFound && !dateMacroFound) {
    return PatternMacro::TIMESTAMP;
  }

  return nebula::meta::PatternMacro::INVALID;
}
} // namespace meta
} // namespace nebula
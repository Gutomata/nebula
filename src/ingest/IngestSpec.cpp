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

#include "IngestSpec.h"

#include <gflags/gflags.h>

#include "common/Evidence.h"
#include "execution/BlockManager.h"
#include "execution/meta/TableService.h"
#include "meta/TestTable.h"
#include "storage/CsvReader.h"
#include "storage/NFS.h"
#include "storage/ParquetReader.h"
#include "type/Serde.h"

// TODO(cao) - system wide enviroment configs should be moved to cluster config to provide
// table-wise customization
DEFINE_string(NTEST_LOADER, "NebulaTest", "define the loader name for loading nebula test data");
DEFINE_uint64(NBLOCK_MAX_ROWS, 50000, "max rows per block");

/**
 * We will sync etcd configs for cluster info into this memory object
 * To understand cluster status - total nodes.
 */
namespace nebula {
namespace ingest {

using nebula::common::Evidence;
using nebula::execution::BlockManager;
using nebula::execution::io::BatchBlock;
using nebula::execution::io::BlockLoader;
using nebula::execution::meta::TableService;
using nebula::memory::Batch;
using nebula::meta::BlockSignature;
using nebula::meta::DataSource;
using nebula::meta::Table;
using nebula::meta::TableSpecPtr;
using nebula::meta::TestTable;
using nebula::meta::TimeSpec;
using nebula::meta::TimeType;
using nebula::storage::CsvReader;
using nebula::storage::ParquetReader;
using nebula::surface::RowCursor;
using nebula::surface::RowData;
using nebula::type::LongType;
using nebula::type::TypeSerializer;

static constexpr auto LOADER_SWAP = "Swap";
static constexpr auto LOADER_ROLL = "Roll";

// load some nebula test data into current process
void loadNebulaTestData(const TableSpecPtr& table, const std::string& spec) {
  // load test data to run this query
  auto bm = BlockManager::init();

  // set up a start and end time for the data set in memory
  // (NOTE) table->max_hr is not serialized, it will be 0
  auto start = table->timeSpec.unixTimeValue;
  auto end = start + Evidence::HOUR_SECONDS * table->max_hr;

  // let's plan these many data std::thread::hardware_concurrency()
  TestTable testTable;
  auto numBlocks = std::thread::hardware_concurrency();
  auto window = (end - start) / numBlocks;
  for (unsigned i = 0; i < numBlocks; i++) {
    size_t begin = start + i * window;
    bm->add(nebula::meta::BlockSignature{
      testTable.name(), i++, begin, begin + window, spec });
  }
}

bool IngestSpec::work() noexcept {
  // TODO(cao) - refator this to have better hirachy for different ingest types.
  const auto& loader = table_->loader;
  if (loader == FLAGS_NTEST_LOADER) {
    loadNebulaTestData(table_, id_);
    return true;
  }

  // either swap, they are reading files
  if (loader == LOADER_SWAP) {
    return this->loadSwap();
  }

  // if roll, they are reading files
  if (loader == LOADER_ROLL) {
    return this->loadRoll();
  }

  // can not hanlde other loader type yet
  return false;
}

std::vector<BatchBlock> IngestSpec::load() noexcept {
  // TODO(cao) - columar format reader (parquet) should be able to
  // access cloud storage directly to save networkbandwidth, but right now
  // we only can download it to local temp file and read it
  auto fs = nebula::storage::makeFS("s3", domain_);

  // id is the file path, copy it from s3 to a local folder
  std::string tmpFile = fs->copy(id_);

  // check if data blocks with the same ingest ID exists
  // since it is a swap loader, we will remove those blocks
  std::vector<BatchBlock> blocks = this->ingest(tmpFile);

  // NOTE: assuming tmp file is created by mkstemp API
  // we unlink it for os to recycle it (linux), ignoring the result
  // https://stackoverflow.com/questions/32445579/when-a-file-created-with-mkstemp-is-deleted
  unlink(tmpFile.c_str());

  // swap each of the blocks into block manager
  // as long as they share the same table / spec
  return blocks;
}

bool IngestSpec::loadSwap() noexcept {
  if (table_->source == DataSource::S3) {
    // load current
    auto blocks = this->load();
    auto bm = BlockManager::init();
    for (BatchBlock& b : blocks) {
      // remove blocks that shares the same spec / table
      bm->removeSameSpec(b.signature());
    }

    // move all new blocks in
    bm->add(std::move(blocks));
    return true;
  }

  return false;
}

bool IngestSpec::loadRoll() noexcept {
  if (table_->source == DataSource::S3) {
    // load current
    auto blocks = this->load();
    auto bm = BlockManager::init();
    // move all new blocks in
    bm->add(std::move(blocks));
    return true;
  }

  return false;
}

// row wrapper to translate "date" string into reserved "_time_" column
class RowWrapperWithTime : public nebula::surface::RowData {
public:
  RowWrapperWithTime(std::function<int64_t(const RowData*)> timeFunc)
    : timeFunc_{ std::move(timeFunc) } {}
  ~RowWrapperWithTime() = default;
  bool set(const RowData* row) {
    row_ = row;
    return true;
  }

// raw date to _time_ columm in ingestion time
#define TRANSFER(TYPE, FUNC)                           \
  TYPE FUNC(const std::string& field) const override { \
    return row_->FUNC(field);                          \
  }

  TRANSFER(bool, readBool)
  TRANSFER(int8_t, readByte)
  TRANSFER(int16_t, readShort)
  TRANSFER(int32_t, readInt)
  TRANSFER(std::string_view, readString)
  TRANSFER(float, readFloat)
  TRANSFER(double, readDouble)
  TRANSFER(std::unique_ptr<nebula::surface::ListData>, readList)
  TRANSFER(std::unique_ptr<nebula::surface::MapData>, readMap)

  bool isNull(const std::string& field) const override {
    if (UNLIKELY(field == Table::TIME_COLUMN)) {
      // timestamp in string 2016-07-15 14:38:03
      return false;
    }

    return row_->isNull(field);
  }

  // _time_ is in long type and it's coming from date string
  int64_t readLong(const std::string& field) const override {
    if (UNLIKELY(field == Table::TIME_COLUMN)) {
      // timestamp in string 2016-07-15 14:38:03
      return timeFunc_(row_);
    }

    return row_->readLong(field);
  }

private:
  std::function<int64_t(const RowData*)> timeFunc_;
  const RowData* row_;
};

std::vector<BatchBlock> IngestSpec::ingest(const std::string& file) noexcept {
  // TODO(cao) - support column selection in ingestion and expand time column
  // to other columns for simple transformation
  // but right now, we're expecting the same schema of data

  // get table schema and create a table
  auto schema = TypeSerializer::from(table_->schema);

  // list all columns describing the current file
  std::vector<std::string> columns;
  columns.reserve(schema->size());
  for (size_t i = 0, size = schema->size(); i < size; ++i) {
    auto type = schema->childType(i);
    columns.push_back(type->name());
  }

  // based on time spec, we need to replace or append time column
  auto timeType = table_->timeSpec.type;
  std::function<int64_t(const RowData*)> timeFunc;

  // static time spec
  switch (timeType) {
  case TimeType::STATIC: {
    schema->addChild(LongType::createTree(Table::TIME_COLUMN));
    timeFunc = [value = table_->timeSpec.unixTimeValue](const RowData*) { return value; };
    break;
  }
  case TimeType::CURRENT: {
    schema->addChild(LongType::createTree(Table::TIME_COLUMN));
    timeFunc = [](const RowData*) { return Evidence::unix_timestamp(); };
    break;
  }
  case TimeType::COLUMN: {
    const auto& ts = table_->timeSpec;
    schema->remove(ts.colName);
    schema->addChild(LongType::createTree(Table::TIME_COLUMN));
    // TODO(cao) - currently only support string column with time pattern
    // we should be able to support other types such as column is number
    timeFunc = [&ts](const RowData* r) {
      return Evidence::time(r->readString(ts.colName), ts.pattern);
    };
    break;
  }
  case TimeType::MACRO: {
    schema->addChild(LongType::createTree(Table::TIME_COLUMN));
    const auto& ts = table_->timeSpec;
    // TODO(cao) - support only one macro for now, need to generalize it
    if (ts.pattern == "date") {
      timeFunc = [d = mdate_](const RowData*) { return d; };
    } else {
      timeFunc = [](const RowData*) { return 0; };
    }
    break;
  }
  default: {
    LOG(ERROR) << "Unsupported time type: " << (int)timeType;
    return {};
  }
  }

  auto table = std::make_shared<Table>(table_->name, schema, table_->columnProps);

  // enroll the table in case it is the first time
  TableService::singleton()->enroll(table);

  size_t blockId = 0;

  // load the data into batch based on block.id * 50000 as offset so that we can keep every 50K rows per block
  LOG(INFO) << "Ingesting from " << file;

  // depends on the type
  std::unique_ptr<RowCursor> source = nullptr;
  if (table_->format == "csv") {
    source = std::make_unique<CsvReader>(file, '\t', columns);
  } else if (table_->format == "parquet") {
    // schema is modified with time column, we need original schema here
    source = std::make_unique<ParquetReader>(file, TypeSerializer::from(table_->schema));
  } else {
    LOG(ERROR) << "Unsupported file format: " << table_->format;
    return {};
  }

  // limit at 1b on single host
  const size_t bRows = FLAGS_NBLOCK_MAX_ROWS;
  auto batch = std::make_shared<Batch>(*table, bRows);
  RowWrapperWithTime rw{ std::move(timeFunc) };
  std::pair<size_t, size_t> range{ std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::min() };

  // TODO(cao) - make a better size estimation to understand total blocks to have
  std::vector<BatchBlock> blocks;
  blocks.reserve(32);

  // a lambda to build batch block
  auto makeBlock = [&table, &range, spec = id_](size_t bid, std::shared_ptr<Batch> b) {
    return BlockLoader::from(
      // build up a block signature with table name, sequence and spec
      BlockSignature{
        table->name(),
        bid,
        range.first,
        range.second,
        spec },
      b);
  };

  while (source->hasNext()) {
    auto& row = source->next();
    rw.set(&row);

    // if this is already full
    if (batch->getRows() >= bRows) {
      // move it to the manager and erase it from the map
      blocks.push_back(makeBlock(blockId++, batch));

      // make a new batch
      batch = std::make_shared<Batch>(*table, bRows);
      range = { std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::min() };
    }

    // update time range before adding the row to the batch
    // get time column value
    size_t time = rw.readLong(Table::TIME_COLUMN);
    if (time < range.first) {
      range.first = time;
    }

    if (time > range.second) {
      range.second = time;
    }

    // add a new entry
    batch->add(rw);
  }

  // move all blocks in map into block manager
  if (batch->getRows() > 0) {
    blocks.push_back(makeBlock(blockId++, batch));
  }

  // return all blocks built up so far
  return blocks;
}

} // namespace ingest
} // namespace nebula
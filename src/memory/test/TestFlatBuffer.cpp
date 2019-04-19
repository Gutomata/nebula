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

#include "gtest/gtest.h"
#include <glog/logging.h>
#include <valarray>
#include "common/Memory.h"
#include "fmt/format.h"
#include "memory/keyed/FlatBuffer.h"
#include "meta/TestTable.h"
#include "surface/DataSurface.h"
#include "surface/StaticData.h"
#include "type/Serde.h"

/**
 * Flat Buffer is used to store / compute run time data. 
 * Test its interfaces and functions here.
 */
namespace nebula {
namespace memory {
namespace test {

using nebula::common::Evidence;
using nebula::memory::keyed::FlatBuffer;
using nebula::surface::MockRowData;
using nebula::surface::RowData;
using nebula::type::TypeSerializer;

// print single row as string.
static constexpr auto line = [](const RowData& r) {
  std::string s;
  if (!r.isNull("items")) {
    const auto list = r.readList("items");
    for (auto k = 0; k < list->getItems(); ++k) {
      s += (list->isNull(k) ? "NULL" : list->readString(k)) + ",";
    }
  }

  return fmt::format("({0}, {1}, [{2}], {3})",
                     r.isNull("id") ? 0 : r.readInt("id"),
                     r.isNull("event") ? "NULL" : r.readString("event"),
                     s,
                     r.isNull("flag") ? true : r.readBool("flag"));
};

TEST(FlatBufferTest, TestFlatBufferWrite) {
  // should be a covariant of TypeNode shared_ptr<RowType> -> shared_ptr<TypeBase>
  auto schema = TypeSerializer::from(nebula::meta::TestTable::schema());

  // initialize a flat row with given schema
  FlatBuffer fb(schema);

  // add 10 rows
  constexpr auto rows2test = 1024;
  auto seed = Evidence::unix_timestamp();
  MockRowData row(seed);
  std::vector<nebula::surface::StaticRow> rows;
  // fill rows
  for (auto i = 0; i < rows2test; ++i) {
    rows.push_back({ row.readInt("id"),
                     row.readString("event"),
                     i % 3 != 0 ? nullptr : row.readList("items"),
                     // row.isNull("items") ? nullptr : row.readList("items"),
                     row.readBool("flag") });
  }

  LOG(INFO) << "Data was generated with seed: " << seed;

  // write the data into the flat buffer
  for (auto i = 0; i < rows2test; ++i) {
    fb.add(rows[i]);
  }

  // read it back
  EXPECT_EQ(fb.getRows(), rows2test);
  LOG(INFO) << "Flat buffer has rows:" << fb.getRows();
  for (auto i = 0; i < rows2test; ++i) {
    const auto& r = fb.row(i);
    EXPECT_EQ(line(r), line(rows[i]));
  }
}

TEST(FlatBufferTest, TestRollback) {
  // should be a covariant of TypeNode shared_ptr<RowType> -> shared_ptr<TypeBase>
  auto schema = TypeSerializer::from(nebula::meta::TestTable::schema());

  // initialize a flat row with given schema
  FlatBuffer fb(schema);

  // add 10 rows
  constexpr auto rows2test = 5;
  auto seed = Evidence::unix_timestamp();
  MockRowData row(seed);

  // add 5 rows
  for (auto i = 0; i < rows2test; ++i) {
    fb.add(row);
  }

  std::vector<std::string> lines;
  lines.reserve(rows2test);
  for (auto i = 0; i < rows2test; ++i) {
    lines.push_back(line(fb.row(i)));
  }

  EXPECT_EQ(fb.getRows(), rows2test);
  // rollback last one
  fb.rollback();
  EXPECT_EQ(fb.getRows(), rows2test - 1);

  // rollback every new row
  for (auto i = 0; i < rows2test; ++i) {
    fb.add(row);
    fb.rollback();
  }

  // last row is different
  fb.add(row);

  EXPECT_EQ(fb.getRows(), rows2test);
  for (auto i = 0; i < rows2test - 1; ++i) {
    EXPECT_EQ(line(fb.row(i)), lines[i]);
  }
}

} // namespace test
} // namespace memory
} // namespace nebula
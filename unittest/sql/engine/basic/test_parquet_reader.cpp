/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/basic/file_sql/parquet_reader.h"
#include <cstdio>
using namespace oceanbase::sql::file_sql;
static void check_parquet_fixture(const std::filesystem::path &path, bool mixed)
{
  const std::vector<std::size_t> selected = mixed ? std::vector<std::size_t>{0,1,2,3,4,5} : std::vector<std::size_t>{0};
  ParquetReader reader(path,path.parent_path(),selected);
  std::vector<CsvField> row;
  long long count = 0, sum = 0;
  const std::vector<std::string> names{"A","B","A","C","C","D"};
  while (reader.next(row)) {
    if (mixed) {
      if (row[1].text != names.at(static_cast<std::size_t>(count))) { throw std::runtime_error("dictionary row mismatch"); }
      if (count == 0 && (!row[4].has_temporal_value || row[4].temporal_value != 0)) { throw std::runtime_error("DATE value mismatch"); }
      if (count == 1 && (!row[5].has_temporal_value || row[5].temporal_value != 1234567)) { throw std::runtime_error("DATETIME precision mismatch"); }
      if (count == 2 && (!row[2].text.empty() || row[2].quoted)) { throw std::runtime_error("nullable boolean mismatch"); }
    }
    ++count;
    if (!row[0].text.empty()) { sum += std::stoll(row[0].text); }
  }
  if (count != (mixed ? 6 : 7) || sum != (mixed ? 13 : 21)) { throw std::runtime_error("Parquet fixture values mismatch"); }
  ParquetReader count_only(path,path.parent_path(),{});
  long long metadata_count = 0;
  while (count_only.next(row)) { ++metadata_count; }
  if (metadata_count != count) { throw std::runtime_error("zero-column projection count mismatch"); }
}
static void check_parquet_encodings()
{
  if (parquet_detail::snappy(std::string("\x03\x08" "abc",5)) != "abc"
      || parquet_detail::snappy(std::string("\x06\x08" "abc\x0a\x03\x00",8)) != "abcabc") {
    throw std::runtime_error("Snappy literal/copy mismatch");
  }
  const auto bits = parquet_detail::hybrid(std::string("\x03\x55",2),1,8);
  if (bits != std::vector<unsigned>{1,0,1,0,1,0,1,0}) { throw std::runtime_error("bit-packed level mismatch"); }
  bool rejected = false;
  try { parquet_detail::snappy(std::string("\x01\x01\x00",3)); }
  catch (const std::runtime_error &) { rejected = true; }
  if (!rejected) { throw std::runtime_error("invalid Snappy copy accepted"); }
}
#ifdef FILE_SQL_STANDALONE_TEST
int main(int argc, char **argv)
{
  try {
    if (argc != 2 && argc != 3) { throw std::runtime_error("pass a Parquet fixture path and optional mixed flag"); }
    check_parquet_encodings();
    check_parquet_fixture(std::filesystem::absolute(path_from_utf8(argv[1])),argc == 3);
    std::puts("Parquet fixture and encoding checks passed");
    return 0;
  } catch (const std::exception &error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
#else
#include <gtest/gtest.h>
TEST(FileSqlParquet, snappy_and_levels) { check_parquet_encodings(); }
TEST(FileSqlParquet, multiple_groups_nulls_and_dictionary)
{
  check_parquet_fixture(std::filesystem::path(__FILE__).parent_path()/"fixtures"/"mixed_snappy.parquet",true);
}
#endif

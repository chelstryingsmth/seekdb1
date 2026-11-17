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

#include "sql/engine/basic/file_sql/csv_output.h"
#include <fstream>
#define CHECK(condition) do { if (!(condition)) { throw std::runtime_error("check failed: " #condition); } } while (false)
using namespace oceanbase::sql::file_sql;
static void check_csv_output()
{
  const auto directory = std::filesystem::temp_directory_path() / ("seekdb_copy_test_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path,e); } } cleanup{directory};
  const auto destination = directory/"output.csv";
  {
    AtomicCsvOutput output(destination,directory);
    output.write(csv_output_record({{"name",true},{"value",true}},{}));
    output.write(csv_output_record({{"a,\"b\n",true},{"",false}},{}));
    output.commit();
  }
  CHECK(std::filesystem::exists(destination));
  SecureInputStream input(destination,directory);
  CsvReader reader(input); std::vector<CsvField> row;
  CHECK(reader.next(row) && row[0].text == "name");
  CHECK(reader.next(row) && row[0].text == "a,\"b\n" && row[1].text.empty() && !row[1].quoted);
  bool rejected = false;
  try { AtomicCsvOutput existing(destination,directory); } catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
  {
    AtomicCsvOutput aborted(directory/"aborted.csv",directory);
    aborted.write("partial");
  }
  CHECK(!std::filesystem::exists(directory/"aborted.csv"));
  std::size_t files = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) { if (entry.is_regular_file()) { ++files; } }
  CHECK(files == 1);
}
#ifdef FILE_SQL_STANDALONE_TEST
int main() { try { check_csv_output(); return 0; } catch (const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; } }
#else
#include <gtest/gtest.h>
TEST(FileSqlCopy, round_trip_exclusive_publication_and_abort) { check_csv_output(); }
#endif

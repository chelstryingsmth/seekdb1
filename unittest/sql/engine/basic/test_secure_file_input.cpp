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

#include "sql/engine/basic/file_sql/secure_input.h"
#include <chrono>
#include <fstream>
#include <sstream>
#define CHECK(condition) do { if (!(condition)) { throw std::runtime_error("check failed: " #condition); } } while (false)
using namespace oceanbase::sql::file_sql;
static void check_secure_input()
{
  const auto directory = std::filesystem::temp_directory_path() /
      ("seekdb_file_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory / "allowed");
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(directory, error); }
  } cleanup{directory};
  { std::ofstream file(directory / "allowed" / "input.csv", std::ios::binary); file << "id\n1\n"; }
  { std::ofstream file(directory / "outside.csv"); file << "secret\n"; }
  SecureInputStream input(directory / "allowed" / "input.csv", directory / "allowed");
  std::string line;
  CHECK(static_cast<bool>(std::getline(input,line)) && line == "id");
  CHECK(static_cast<bool>(std::getline(input,line)) && line == "1");
  bool rejected = false;
  try { SecureInputStream outside(directory / "allowed" / ".." / "outside.csv", directory / "allowed"); }
  catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
  const auto snapshot = FileSnapshot::capture(directory / "allowed" / "input.csv");
  { std::ofstream file(directory / "allowed" / "input.csv", std::ios::app); file << "2\n"; }
  rejected = false;
  try { snapshot.verify(directory / "allowed" / "input.csv"); }
  catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
  const auto same_size = FileSnapshot::capture(directory / "allowed" / "input.csv");
  { std::ofstream file(directory / "allowed" / "input.csv",std::ios::binary); file << "id\n9\n8\n"; }
  std::filesystem::last_write_time(directory / "allowed" / "input.csv",same_size.modified);
  rejected = false;
  try { same_size.verify(directory / "allowed" / "input.csv"); }
  catch (const std::exception &) { rejected = true; }
  CHECK(rejected);
}
#ifdef FILE_SQL_STANDALONE_TEST
int main() { try { check_secure_input(); return 0; } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; } }
#else
#include <gtest/gtest.h>
TEST(FileSqlSecureInput, containment_read_and_changes) { check_secure_input(); }
#endif

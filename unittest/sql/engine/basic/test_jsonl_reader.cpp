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

#include "sql/engine/basic/file_sql/jsonl_reader.h"
#include <sstream>
#define CHECK(condition) do { if (!(condition)) { throw std::runtime_error("check failed: " #condition); } } while (false)
using namespace oceanbase::sql::file_sql;
static void check_jsonl_reader()
{
  std::istringstream input("{\"id\":1,\"name\":\"A\"}\n{\"id\":2,\"score\":2.5}\n{\"id\":3,\"score\":null}\n");
  const auto schema = infer_jsonl(input);
  CHECK(schema.rows == 3);
  CHECK((schema.names == std::vector<std::string>{"id","name","score"}));
  CHECK(schema.types[0] == ScalarType::Bigint && schema.types[2] == ScalarType::Double);
  for (const auto &text : {"[]", "{\"x\":[]}", "{\"x\":{}}", "{\"x\":1,\"x\":2}", "{broken}"}) {
    std::istringstream bad(text);
    JsonRecord record;
    bool rejected = false;
    try { read_json_record(bad, record); } catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected);
  }
  std::istringstream missing("{\"x\":null,\"s\":\"\",\"n\":9223372036854775808}");
  JsonRecord record;
  CHECK(read_json_record(missing, record));
  CHECK(record.types[0] == ScalarType::Null && record.fields[1].quoted);
  CHECK(record.fields[1].text.empty() && record.fields[2].text == "9223372036854775808");
  CHECK(!read_json_record(missing, record));
}
#ifdef FILE_SQL_STANDALONE_TEST
int main() { try { check_jsonl_reader(); return 0; } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; } }
#else
#include <gtest/gtest.h>
TEST(FileSqlJsonlReader, union_types_and_invalid_input) { check_jsonl_reader(); }
#endif

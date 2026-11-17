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

#include "sql/engine/basic/file_sql/csv_schema.h"
#define CHECK(condition) do { if (!(condition)) { throw std::runtime_error("check failed: " #condition); } } while (false)
#include <sstream>
using namespace oceanbase::sql::file_sql;
static void check_csv_reader()
{
  CHECK(infer_scalar({"9223372036854775807"}) == ScalarType::Bigint);
  CHECK(infer_scalar({"9223372036854775808"}) == ScalarType::Varchar);
  CHECK(infer_scalar({"2024-02-29"}) == ScalarType::Date);
  CHECK(infer_scalar({"2025-02-29"}) == ScalarType::Varchar);
  CHECK(infer_scalar({"2026-11-17T23:59:59"}) == ScalarType::Datetime);
  CHECK(infer_scalar({"2026-11-17T24:00:00"}) == ScalarType::Varchar);
  CHECK(infer_scalar({"", true}) == ScalarType::Varchar);
  CHECK(widen(ScalarType::Bigint, ScalarType::Double) == ScalarType::Double);
  std::istringstream schema_input("id,value\n1,\n2,2.5\n3,3\n");
  const auto schema = infer_csv(schema_input);
  CHECK(schema.rows == 3 && schema.types[0] == ScalarType::Bigint && schema.types[1] == ScalarType::Double);
  std::istringstream bom("\xEF\xBB\xBF\"id\",name\r\n1,x\r\n");
  const auto bom_schema = infer_csv(bom);
  CHECK(bom_schema.names[0] == "id" && bom_schema.rows == 1);
  std::istringstream custom("id;name\r\n1;'it\\'s'\r\n2;NULL\r\n");
  const auto custom_schema = infer_csv(custom,true,';',static_cast<char>(39),static_cast<char>(92),"NULL");
  CHECK(custom_schema.rows == 2 && custom_schema.nullable[1]);
  std::istringstream quoted_crlf("\"a\r\nb\"\n");
  CsvReader normalized(quoted_crlf);
  std::vector<CsvField> normalized_row;
  CHECK(normalized.next(normalized_row) && normalized_row[0].text == "a\nb");
  const auto names = csv_column_names({{"id"}, {"id"}, {"id__2"}, {""}});
  CHECK((names == std::vector<std::string>{"id", "id__2", "id__2__2", "column4"}));
  bool disabled = false;
  try { authorize_input("missing", ".", false); } catch (const std::runtime_error &) { disabled = true; }
  CHECK(disabled);
  std::istringstream input("id,note\r\n1,\"hello,\nworld\"\r\n2,\"say \"\"hi\"\"\"\n3,\n");
  CsvReader reader(input);
  std::vector<CsvField> row;
  CHECK(reader.next(row) && row.size() == 2 && row[0].text == "id");
  CHECK(reader.next(row) && row[1].text == "hello,\nworld" && row[1].quoted);
  CHECK(reader.next(row) && row[1].text == "say \"hi\"");
  CHECK(reader.next(row) && row[1].text.empty() && !row[1].quoted);
  CHECK(!reader.next(row));
  for (const auto &text : {"\"unterminated", "\"closed\"garbage", "bad\"quote"}) {
    std::istringstream bad(text);
    CsvReader bad_reader(bad);
    bool rejected = false;
    try { bad_reader.next(row); } catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected);
  }
  std::istringstream huge("12345");
  CsvReader bounded(huge, ',', 4);
  bool rejected = false;
  try { bounded.next(row); } catch (const std::runtime_error &) { rejected = true; }
  CHECK(rejected);
  std::istringstream empty("");
  CsvReader empty_reader(empty);
  CHECK(!empty_reader.next(row));
}

#ifdef FILE_SQL_STANDALONE_TEST
int main() { try { check_csv_reader(); return 0; } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; } }
#else
#include <gtest/gtest.h>
TEST(FileSqlCsvReader, streaming_records_and_errors) { check_csv_reader(); }
#endif
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

namespace oceanbase::sql::file_sql {

inline bool valid_utf8(const std::string &text)
{
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i++]);
    if (lead < 0x80) { continue; }
    unsigned code = 0;
    unsigned count = 0;
    unsigned minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) { code = lead & 0x1F; count = 1; minimum = 0x80; }
    else if (lead >= 0xE0 && lead <= 0xEF) { code = lead & 0x0F; count = 2; minimum = 0x800; }
    else if (lead >= 0xF0 && lead <= 0xF4) { code = lead & 7; count = 3; minimum = 0x10000; }
    else { return false; }
    if (count > text.size()-i) { return false; }
    while (count-- > 0) {
      const unsigned char ch = static_cast<unsigned char>(text[i++]);
      if ((ch & 0xC0) != 0x80) { return false; }
      code = (code << 6) | (ch & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) { return false; }
  }
  return true;
}

struct CsvField {
  std::string text;
  bool quoted = false;
  std::int64_t temporal_value = 0;
  bool has_temporal_value = false;
};

// Reads one record at a time; memory is bounded independently of file size.
class CsvReader {
public:
  explicit CsvReader(std::istream &input, char delimiter = ',',
                     std::size_t max_record_bytes = 16 * 1024 * 1024, char quote = '"', char escape = '"')
      : input_(input), delimiter_(delimiter), max_record_bytes_(max_record_bytes), quote_(quote), escape_(escape)
  {
    if (delimiter == '\r' || delimiter == '\n' || delimiter == quote || delimiter == '\0' || quote == '\0' || quote == '\r' || quote == '\n'
        || escape == '\0' || escape == '\r' || escape == '\n'
        || static_cast<unsigned char>(delimiter) >= 128 || static_cast<unsigned char>(quote) >= 128
        || static_cast<unsigned char>(escape) >= 128 || max_record_bytes == 0) {
      throw std::invalid_argument("invalid CSV reader options");
    }
  }

  bool next(std::vector<CsvField> &fields)
  {
    if (first_) {
      first_ = false;
      for (unsigned i = 0; i < 3; ++i) {
        const int ch = input_.get();
        if (ch == std::char_traits<char>::eof()) { break; }
        prefix_.push_back(static_cast<char>(ch));
      }
      if (prefix_ == "\xEF\xBB\xBF") { prefix_.clear(); }
    }
    fields.clear();
    CsvField field;
    bool in_quotes = false;
    bool closed_quote = false;
    bool seen = false;
    std::size_t bytes = 0;
    for (;;) {
      const int next_char = read_char();
      if (next_char == std::char_traits<char>::eof()) {
        if (input_.bad() || (!input_.eof() && input_.fail())) {
          fail("input read failure");
        }
        if (in_quotes) { fail("unterminated quoted field"); }
        if (!seen) { return false; }
        append_field(fields, std::move(field));
        ++record_;
        return true;
      }
      seen = true;
      if (++bytes > max_record_bytes_) { fail("record exceeds byte limit"); }
      const char ch = static_cast<char>(next_char);
      if (in_quotes) {
        if (escape_ != quote_ && ch == escape_) {
          const int escaped = read_char();
          if (escaped == std::char_traits<char>::eof()) { fail("truncated CSV escape"); }
          if (++bytes > max_record_bytes_) { fail("record exceeds byte limit"); }
          field.text.push_back(static_cast<char>(escaped));
        } else if (ch == quote_) {
          in_quotes = false;
          closed_quote = true;
        } else {
          if (ch == '\r' && peek_char() == '\n') {
            read_char();
            if (++bytes > max_record_bytes_) { fail("record exceeds byte limit"); }
            field.text.push_back('\n');
          } else {
            field.text.push_back(ch);
          }
        }
      } else if (closed_quote && escape_ == quote_ && ch == quote_) {
        field.text.push_back(quote_);
        in_quotes = true;
        closed_quote = false;
      } else if (ch == delimiter_) {
        append_field(fields, std::move(field));
        field = CsvField{};
        closed_quote = false;
      } else if (ch == '\n' || ch == '\r') {
        if (ch == '\r' && peek_char() == '\n') { read_char(); }
        append_field(fields, std::move(field));
        ++record_;
        return true;
      } else if (closed_quote) {
        fail("unexpected character after closing quote");
      } else if (ch == quote_) {
        if (!field.text.empty()) { fail("quote inside unquoted field"); }
        field.quoted = true;
        in_quotes = true;
      } else {
        field.text.push_back(ch);
      }
    }
  }

private:
  int read_char()
  {
    if (prefix_position_ < prefix_.size()) { return static_cast<unsigned char>(prefix_[prefix_position_++]); }
    return input_.get();
  }
  int peek_char()
  {
    if (prefix_position_ < prefix_.size()) { return static_cast<unsigned char>(prefix_[prefix_position_]); }
    return input_.peek();
  }
  void append_field(std::vector<CsvField> &fields, CsvField &&field) const
  {
    if (fields.size() >= 4096) { fail("column limit exceeded"); }
    if (!valid_utf8(field.text)) { fail("invalid UTF-8 field"); }
    fields.push_back(std::move(field));
  }
  [[noreturn]] void fail(const char *message) const
  {
    throw std::runtime_error("CSV record " + std::to_string(record_) + ": " + message);
  }
  std::istream &input_;
  char delimiter_;
  std::size_t max_record_bytes_;
  std::size_t record_ = 1;
  char quote_;
  char escape_;
  bool first_ = true;
  std::string prefix_;
  std::size_t prefix_position_ = 0;
};

} // namespace oceanbase::sql::file_sql

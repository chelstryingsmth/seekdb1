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
#include "csv_schema.h"
#include <map>
#include <algorithm>
#include <array>
#include <sstream>
#include <cstring>

namespace oceanbase::sql::file_sql {
namespace parquet_detail {
inline std::uint64_t little(const char *bytes, std::size_t count)
{
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < count; ++i) { result |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (8*i); }
  return result;
}
struct Node {
  int type = 0;
  std::int64_t integer = 0;
  std::string text;
  std::vector<Node> list;
  std::map<int, Node> fields;
  const Node &at(int key) const
  {
    const auto found = fields.find(key);
    if (found == fields.end()) { throw std::runtime_error("missing Parquet metadata field"); }
    return found->second;
  }
  std::int64_t integer_at(int key) const
  {
    const auto &field = at(key);
    if (field.type < 3 || field.type > 6) { throw std::runtime_error("invalid required Parquet integer type"); }
    return field.integer;
  }
  std::int64_t number(int key, std::int64_t fallback = 0) const
  {
    const auto found = fields.find(key);
    if (found == fields.end()) { return fallback; }
    if (found->second.type < 1 || found->second.type > 6) { throw std::runtime_error("invalid Parquet integer field type"); }
    return found->second.integer;
  }
};
class CompactReader {
public:
  explicit CompactReader(std::istream &input, std::size_t limit = 16*1024*1024) : input_(input), left_(limit) {}
  unsigned byte()
  {
    if (left_ == 0) { throw std::runtime_error("Parquet metadata limit exceeded"); }
    --left_;
    const int ch = input_.get();
    if (ch == std::char_traits<char>::eof()) { throw std::runtime_error("truncated Parquet metadata"); }
    return static_cast<unsigned char>(ch);
  }
  std::uint64_t varint()
  {
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      const unsigned ch = byte();
      if (shift == 63 && ch > 1) { throw std::runtime_error("Parquet varint overflow"); }
      result |= std::uint64_t(ch & 127) << shift;
      if (ch < 128) { return result; }
    }
    throw std::runtime_error("Parquet varint overflow");
  }
  Node structure(unsigned depth = 0)
  {
    if (depth > 32) { throw std::runtime_error("Parquet metadata nesting limit"); }
    Node result; result.type = 12;
    int field = 0;
    while (true) {
      const unsigned header = byte();
      if (header == 0) { return result; }
      const unsigned delta = header >> 4;
      if (delta != 0) { field += delta; }
      else { field = static_cast<int>(signed_integer()); }
      if (field <= 0 || result.fields.size() >= 1024 || result.fields.count(field)) {
        throw std::runtime_error("invalid Parquet field id");
      }
      result.fields.emplace(field, value(header & 15, depth+1));
    }
  }
private:
  std::int64_t signed_integer()
  {
    const std::uint64_t number = varint();
    return static_cast<std::int64_t>(number >> 1) ^ -static_cast<std::int64_t>(number & 1);
  }
  Node value(unsigned type, unsigned depth)
  {
    if (depth > 32) { throw std::runtime_error("Parquet metadata nesting limit"); }
    if (++nodes_ > 65536) { throw std::runtime_error("Parquet metadata node limit exceeded"); }
    Node node; node.type = type;
    if (type == 1 || type == 2) { node.integer = type == 1; }
    else if (type == 3) { node.integer = static_cast<std::int8_t>(byte()); }
    else if (type >= 4 && type <= 6) { node.integer = signed_integer(); }
    else if (type == 7) { for (int i = 0; i < 8; ++i) { byte(); } }
    else if (type == 8) {
      const auto length = varint();
      if (length > left_) { throw std::runtime_error("Parquet string limit exceeded"); }
      node.text.reserve(static_cast<std::size_t>(length));
      for (std::size_t i = 0; i < length; ++i) { node.text.push_back(static_cast<char>(byte())); }
    } else if (type == 9 || type == 10) {
      const unsigned header = byte();
      const auto count = header >> 4 == 15 ? varint() : header >> 4;
      if (count > 65536 || count > left_) { throw std::runtime_error("Parquet list limit exceeded"); }
      if (count > 65536-reserved_nodes_) { throw std::runtime_error("Parquet metadata allocation limit exceeded"); }
      reserved_nodes_ += static_cast<std::size_t>(count);
      node.list.reserve(static_cast<std::size_t>(count));
      for (std::size_t i = 0; i < count; ++i) {
        const unsigned element = (header & 15) == 1 || (header & 15) == 2 ? byte() : header & 15;
        node.list.push_back(value(element,depth+1));
      }
    } else if (type == 11) {
      const auto count = varint();
      if (count > 100000 || count > left_/2) { throw std::runtime_error("Parquet map limit exceeded"); }
      if (count != 0) {
        const unsigned types = byte();
        for (std::size_t i = 0; i < count; ++i) { value(types >> 4,depth+1); value(types & 15,depth+1); }
      }
    } else if (type == 12) { node = structure(depth+1); }
    else { throw std::runtime_error("unsupported Parquet compact type"); }
    return node;
  }
  std::istream &input_;
  std::size_t left_;
  std::size_t nodes_ = 0, reserved_nodes_ = 0;
};

inline std::string read_bytes(std::istream &input, std::size_t count)
{
  if (count > 32*1024*1024) { throw std::runtime_error("Parquet buffer limit exceeded"); }
  std::string bytes(count,'\0');
  if (count != 0) { input.read(bytes.data(), count); }
  if (static_cast<std::size_t>(input.gcount()) != count && count != 0) { throw std::runtime_error("truncated Parquet bytes"); }
  return bytes;
}
inline Node footer(std::istream &input)
{
  input.seekg(0,std::ios::end);
  const auto size = input.tellg();
  if (size < std::streamoff(12)) { throw std::runtime_error("invalid Parquet file size"); }
  input.seekg(0); if (read_bytes(input,4) != "PAR1") { throw std::runtime_error("invalid Parquet magic"); }
  input.seekg(-8,std::ios::end);
  const auto trailer = read_bytes(input,8);
  const auto length = little(trailer.data(),4);
  if (trailer.substr(4) != "PAR1" || length > 16*1024*1024
      || length > static_cast<std::uint64_t>(size)-12) { throw std::runtime_error("invalid Parquet footer"); }
  input.seekg(static_cast<std::streamoff>(size)-8-static_cast<std::streamoff>(length));
  std::istringstream metadata(read_bytes(input,static_cast<std::size_t>(length)),std::ios::binary);
  return CompactReader(metadata).structure();
}
} // namespace parquet_detail
} // namespace oceanbase::sql::file_sql

namespace oceanbase::sql::file_sql {
namespace parquet_detail {
class Bytes {
public:
  explicit Bytes(const std::string &data) : data_(data) {}
  std::uint64_t take(std::size_t size)
  {
    if (size > 8 || size > data_.size()-position) { throw std::runtime_error("truncated Parquet value"); }
    const auto value = little(data_.data()+position,size); position += size; return value;
  }
  std::string text(std::size_t size)
  {
    if (size > data_.size()-position) { throw std::runtime_error("truncated Parquet string"); }
    const auto value = data_.substr(position,size); position += size; return value;
  }
  std::uint64_t varint()
  {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      const auto byte = take(1);
      if (shift == 63 && byte > 1) { throw std::runtime_error("Parquet varint overflow"); }
      value |= (byte & 127) << shift;
      if (byte < 128) { return value; }
    }
    throw std::runtime_error("Parquet varint overflow");
  }
  std::size_t position = 0;
private:
  const std::string &data_;
};
inline std::uint32_t crc32(const std::string &bytes)
{
  static const auto table = [] {
    std::array<std::uint32_t,256> values{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      auto value = i;
      for (unsigned bit = 0; bit < 8; ++bit) { value = (value >> 1) ^ ((value & 1) ? 0xEDB88320u : 0u); }
      values[i] = value;
    }
    return values;
  }();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char byte : bytes) { crc = table[(crc ^ byte) & 255] ^ (crc >> 8); }
  return crc ^ 0xFFFFFFFFu;
}
inline std::string snappy(const std::string &compressed)
{
  Bytes source(compressed);
  const auto size = source.varint();
  if (size > 16*1024*1024) { throw std::runtime_error("Snappy expansion limit exceeded"); }
  std::string output; output.reserve(static_cast<std::size_t>(size));
  while (output.size() < size) {
    const auto tag = source.take(1);
    const unsigned kind = tag & 3;
    std::size_t length = 0, offset = 0;
    if (kind == 0) {
      length = tag >> 2;
      if (length >= 60) { length = static_cast<std::size_t>(source.take(length-59)); }
      ++length;
      if (length > size-output.size()) { throw std::runtime_error("invalid Snappy literal length"); }
      output += source.text(length);
    } else {
      if (kind == 1) { length = 4+((tag >> 2)&7); offset = ((tag & 0xE0)<<3) | source.take(1); }
      else { length = 1+(tag >> 2); offset = static_cast<std::size_t>(source.take(kind == 2 ? 2 : 4)); }
      if (offset == 0 || offset > output.size() || length > size-output.size()) {
        throw std::runtime_error("invalid Snappy copy");
      }
      for (std::size_t i = 0; i < length; ++i) { output.push_back(output[output.size()-offset]); }
    }
  }
  if (source.position != compressed.size()) { throw std::runtime_error("extra Snappy bytes"); }
  return output;
}
inline std::vector<unsigned> hybrid(const std::string &data, unsigned width, std::size_t count)
{
  if (width > 32 || count > 262144) { throw std::runtime_error("Parquet level limit exceeded"); }
  Bytes source(data);
  std::vector<unsigned> output; output.reserve(count);
  while (output.size() < count) {
    const auto header = source.varint();
    if (header == 0) { throw std::runtime_error("empty Parquet RLE run"); }
    if ((header & 1) == 0) {
      const auto value = source.take((width+7)/8);
      if (width < 32 && value >= (std::uint64_t(1) << width)) { throw std::runtime_error("invalid RLE value"); }
      const auto length = std::min<std::uint64_t>(header >> 1,count-output.size());
      output.insert(output.end(),static_cast<std::size_t>(length),static_cast<unsigned>(value));
    } else {
      const auto groups = header >> 1;
      if (groups > 32768) { throw std::runtime_error("Parquet bit-packed run limit exceeded"); }
      const auto bytes = source.text(static_cast<std::size_t>(groups)*width);
      for (std::size_t i = 0; i < groups*8 && output.size() < count; ++i) {
        unsigned value = 0;
        for (unsigned bit = 0; bit < width; ++bit) {
          const auto index = i*width+bit;
          value |= ((static_cast<unsigned char>(bytes[index/8]) >> (index%8)) & 1) << bit;
        }
        output.push_back(value);
      }
    }
  }
  return output;
}
struct Column {
  int physical = 0;
  int converted = -1;
  int length = 0;
  bool optional = false;
};
struct Chunk {
  std::int64_t offset = 0;
  std::int64_t bytes = 0;
  int codec = 0;
};
struct Group {
  std::int64_t rows = 0;
  std::vector<Chunk> chunks;
};
struct Metadata {
  CsvSchema schema;
  std::vector<Column> columns;
  std::vector<Group> groups;
};
inline Metadata metadata(std::istream &input)
{
  const Node root = footer(input);
  Metadata result;
  const auto &elements = root.at(2).list;
  if (elements.empty() || elements.size() > 4097
      || elements[0].number(5) != static_cast<std::int64_t>(elements.size()-1)) {
    throw std::runtime_error("nested or invalid Parquet schema");
  }
  std::set<std::string> names;
  for (std::size_t i = 1; i < elements.size(); ++i) {
    const auto &element = elements[i];
    Column column;
    column.physical = static_cast<int>(element.integer_at(1));
    column.converted = static_cast<int>(element.number(6,-1));
    column.length = static_cast<int>(element.number(2));
    if (element.number(3) < 0 || element.number(3) > 1 || element.number(5) != 0) {
      throw std::runtime_error("nested Parquet columns are unsupported");
    }
    column.optional = element.number(3) == 1;
    const auto &name = element.at(4).text;
    if (name.empty() || !names.insert(name).second || !valid_utf8(name)) { throw std::runtime_error("invalid Parquet column name"); }
    if (column.converted != -1 && column.converted != 0 && column.converted != 4
        && column.converted != 6 && column.converted != 9 && column.converted != 10
        && !(column.converted >= 11 && column.converted <= 18)) {
      throw std::runtime_error("unsupported Parquet logical type");
    }
    if ((column.converted == 6 && column.physical != 1)
        || ((column.converted == 9 || column.converted == 10) && column.physical != 2)) {
      throw std::runtime_error("invalid Parquet temporal physical type");
    }
    ScalarType type;
    if (column.physical == 0) { type = ScalarType::Boolean; }
    else if (column.physical == 1 || column.physical == 2) { type = ScalarType::Bigint; }
    else if (column.physical == 4 || column.physical == 5) { type = ScalarType::Double; }
    else if (column.physical == 6 || column.physical == 7) { type = ScalarType::Varchar; }
    else { throw std::runtime_error("unsupported Parquet physical type"); }
    if (column.converted == 6) { type = ScalarType::Date; }
    else if (column.converted == 9 || column.converted == 10) { type = ScalarType::Datetime; }
    else if (column.converted == 14) { type = ScalarType::Varchar; }
    result.schema.names.push_back(name);
    result.schema.types.push_back(type);
    result.schema.nullable.push_back(column.optional);
    result.schema.temporal_scales.push_back(column.converted == 9 ? 3 : column.converted == 10 ? 6 : 0);
    result.columns.push_back(column);
  }
  const auto total_rows = root.integer_at(3);
  if (total_rows < 0) { throw std::runtime_error("negative Parquet row count"); }
  std::int64_t rows = 0;
  for (const auto &group : root.at(4).list) {
    Group output; output.rows = group.integer_at(3);
    if (output.rows < 0 || output.rows > total_rows-rows) { throw std::runtime_error("invalid Parquet row group count"); }
    rows += output.rows;
    const auto &chunks = group.at(1).list;
    if (chunks.size() != result.columns.size()) { throw std::runtime_error("Parquet column count mismatch"); }
    for (std::size_t i = 0; i < chunks.size(); ++i) {
      if (chunks[i].fields.count(1)) { throw std::runtime_error("external Parquet column files are unsupported"); }
      const auto &column = chunks[i].at(3);
      if (column.integer_at(1) != result.columns[i].physical || column.integer_at(5) != output.rows
          || column.at(3).list.size() != 1 || column.at(3).list[0].text != result.schema.names[i]) {
        throw std::runtime_error("invalid Parquet column metadata");
      }
      Chunk chunk;
      chunk.offset = column.number(11,column.integer_at(9));
      chunk.bytes = column.integer_at(7);
      chunk.codec = static_cast<int>(column.integer_at(4));
      if (chunk.offset < 4 || chunk.bytes < 0 || (chunk.codec != 0 && chunk.codec != 1)) {
        throw std::runtime_error("unsupported Parquet compression or invalid offset");
      }
      output.chunks.push_back(chunk);
    }
    result.groups.push_back(std::move(output));
  }
  if (rows != total_rows) { throw std::runtime_error("Parquet row count mismatch"); }
  result.schema.rows = static_cast<std::size_t>(total_rows);
  return result;
}
inline std::vector<CsvField> plain(const std::string &data, const Column &column, std::size_t count)
{
  if (count > 262144) { throw std::runtime_error("Parquet page value limit exceeded"); }
  Bytes source(data);
  std::vector<CsvField> values; values.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    CsvField field; field.quoted = true;
    std::int64_t integer = 0;
    if (column.physical == 0) {
      if (i/8 >= data.size()) { throw std::runtime_error("truncated Parquet boolean"); }
      field.text = ((static_cast<unsigned char>(data[i/8]) >> (i%8)) & 1) ? "true" : "false";
    } else if (column.physical == 1 || column.physical == 2) {
      const auto raw = source.take(column.physical == 1 ? 4 : 8);
      integer = column.physical == 1 ? static_cast<std::int32_t>(raw) : static_cast<std::int64_t>(raw);
      if (column.converted == 13) { integer = static_cast<std::uint32_t>(raw); }
      field.text = column.converted == 14 ? std::to_string(raw) : std::to_string(integer);
      if (column.converted == 6 || column.converted == 9 || column.converted == 10) {
        if (column.converted == 9 && (integer > INT64_MAX/1000 || integer < INT64_MIN/1000)) {
          throw std::runtime_error("Parquet timestamp overflow");
        }
        field.has_temporal_value = true;
        field.temporal_value = column.converted == 9 ? integer*1000 : integer;
      }
    } else if (column.physical == 4 || column.physical == 5) {
      const auto raw = source.take(column.physical == 4 ? 4 : 8);
      double value;
      if (column.physical == 4) { const auto bits = static_cast<std::uint32_t>(raw); float number; std::memcpy(&number,&bits,4); value = number; }
      else { std::memcpy(&value,&raw,8); }
      if (!std::isfinite(value)) { throw std::runtime_error("nonfinite Parquet value"); }
      char buffer[64]; const auto converted = std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general,std::numeric_limits<double>::max_digits10);
      if (converted.ec != std::errc{}) { throw std::runtime_error("Parquet numeric conversion failed"); }
      field.text.assign(buffer,converted.ptr);
    } else {
      const auto length = column.physical == 6 ? source.take(4) : static_cast<std::uint64_t>(column.length);
      if (length > 16*1024*1024) { throw std::runtime_error("Parquet string limit exceeded"); }
      field.text = source.text(static_cast<std::size_t>(length));
      if (!valid_utf8(field.text)) { throw std::runtime_error("invalid UTF-8 Parquet string"); }
    }
    values.push_back(std::move(field));
  }
  const auto consumed = column.physical == 0 ? (count+7)/8 : source.position;
  if (consumed != data.size()) { throw std::runtime_error("extra Parquet PLAIN value bytes"); }
  return values;
}
} // namespace parquet_detail
} // namespace oceanbase::sql::file_sql

#include "secure_input.h"
#include <memory>
namespace oceanbase::sql::file_sql {
class ParquetReader {
  class ColumnReader {
  public:
    ColumnReader(const std::filesystem::path &path, const std::filesystem::path &root,
                 parquet_detail::Column column, std::size_t &memory)
        : input_(path,root), column_(column), memory_(memory) {}
    ~ColumnReader() { memory_ -= charge_; }
    void begin(const parquet_detail::Chunk &chunk, std::int64_t rows)
    {
      memory_ -= charge_; charge_ = 0;
      std::vector<CsvField>().swap(dictionary_); std::vector<CsvField>().swap(values_);
      std::vector<unsigned>().swap(indices_); std::vector<unsigned>().swap(definitions_);
      dictionary_charge_ = 0;
      chunk_ = chunk;
      input_.clear(); input_.seekg(chunk.offset);
      if (!input_) { throw std::runtime_error("invalid Parquet column offset"); }
      remaining_ = rows; count_ = 0; position_ = 0; value_position_ = 0;
    }
    CsvField next()
    {
      while (position_ == count_) { page(); }
      const bool present = definitions_.empty() || definitions_[position_] == 1;
      ++position_;
      if (!present) { return {}; }
      const auto index = value_position_++;
      if (dictionary_page_) {
        const auto id = indices_.at(index);
        if (id >= dictionary_.size()) { throw std::runtime_error("invalid Parquet dictionary index"); }
        return dictionary_[id];
      }
      return values_.at(index);
    }
  private:
    void reserve_memory(std::size_t bytes)
    {
      if (bytes > 64*1024*1024 || memory_ > 64*1024*1024-bytes) {
        throw std::runtime_error("Parquet scan memory limit exceeded");
      }
      memory_ += bytes; charge_ += bytes;
    }
    void page()
    {
      if (remaining_ <= 0) { throw std::runtime_error("Parquet column ended before its row group"); }
      memory_ -= charge_-dictionary_charge_; charge_ = dictionary_charge_;
      std::vector<CsvField>().swap(values_);
      std::vector<unsigned>().swap(indices_);
      std::vector<unsigned>().swap(definitions_);
      for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        const auto position = static_cast<std::int64_t>(input_.tellg());
        if (position < chunk_.offset || position-chunk_.offset >= chunk_.bytes) {
          throw std::runtime_error("truncated Parquet column chunk");
        }
        const auto header = parquet_detail::CompactReader(input_,65536).structure();
        const auto compressed = header.integer_at(3);
        const auto uncompressed = header.integer_at(2);
        const auto body_position = static_cast<std::int64_t>(input_.tellg());
        if (compressed < 0 || uncompressed < 0 || compressed > 16*1024*1024 || uncompressed > 16*1024*1024
            || body_position < chunk_.offset || compressed > chunk_.bytes-(body_position-chunk_.offset)) {
          throw std::runtime_error("invalid Parquet page size");
        }
        std::string payload = parquet_detail::read_bytes(input_,static_cast<std::size_t>(compressed));
        if (header.fields.count(4) && parquet_detail::crc32(payload) != static_cast<std::uint32_t>(header.integer_at(4))) {
          throw std::runtime_error("Parquet page checksum mismatch");
        }
        const auto type = header.integer_at(1);
        if (type == 1) { continue; }
        const bool v2 = type == 3;
        std::size_t level_bytes = 0;
        if (v2) {
          const auto &info = header.at(8);
          const auto definitions = info.integer_at(5);
          if (definitions < 0 || info.integer_at(6) != 0 || definitions > compressed || definitions > uncompressed) {
            throw std::runtime_error("invalid Parquet v2 level lengths");
          }
          level_bytes = static_cast<std::size_t>(definitions);
          const bool encoded = info.number(7,1) != 0;
          if (encoded && chunk_.codec == 1) {
            payload = payload.substr(0,level_bytes) + parquet_detail::snappy(payload.substr(level_bytes));
          }
        } else if (chunk_.codec == 1) { payload = parquet_detail::snappy(payload); }
        if (payload.size() != static_cast<std::size_t>(uncompressed)) { throw std::runtime_error("Parquet page size mismatch"); }
        if (type == 2) {
          if (!dictionary_.empty() || header.at(7).number(2) != 0) { throw std::runtime_error("invalid Parquet dictionary page"); }
          const auto count = header.at(7).integer_at(1);
          if (count < 0 || count > 262144) { throw std::runtime_error("Parquet dictionary limit exceeded"); }
          const auto bytes = static_cast<std::size_t>(count)*sizeof(CsvField)+payload.size()*2;
          reserve_memory(bytes); dictionary_charge_ += bytes;
          dictionary_ = parquet_detail::plain(payload,column_,static_cast<std::size_t>(count));
          continue;
        }
        if (type != 0 && type != 3) { throw std::runtime_error("unsupported Parquet page type"); }
        const auto &info = header.at(v2 ? 8 : 5);
        const auto count = info.integer_at(1);
        if (count < 0 || count > remaining_ || count > 262144) { throw std::runtime_error("invalid Parquet page row count"); }
        remaining_ -= count; count_ = static_cast<std::size_t>(count); position_ = 0; value_position_ = 0;
        reserve_memory(count_*sizeof(CsvField)+payload.size()*2+count_*8);
        std::size_t offset = 0;
        if (column_.optional) {
          if (v2) {
            definitions_ = parquet_detail::hybrid(payload.substr(0,level_bytes),1,count_);
            offset = level_bytes;
          } else {
            if (info.number(3) != 3 || payload.size() < 4) { throw std::runtime_error("invalid Parquet definition levels"); }
            level_bytes = static_cast<std::size_t>(parquet_detail::little(payload.data(),4));
            if (level_bytes > payload.size()-4) { throw std::runtime_error("truncated Parquet definition levels"); }
            definitions_ = parquet_detail::hybrid(payload.substr(4,level_bytes),1,count_);
            offset = 4+level_bytes;
          }
        } else if (v2 && level_bytes != 0) { throw std::runtime_error("unexpected Parquet definition levels"); }
        std::size_t present = count_;
        if (column_.optional) { present = std::count(definitions_.begin(),definitions_.end(),1u); }
        if (v2 && (info.integer_at(2) != static_cast<std::int64_t>(count_-present)
                   || info.integer_at(3) != count)) { throw std::runtime_error("Parquet v2 row count mismatch"); }
        const auto encoding = info.at(2+(v2 ? 2 : 0)).integer;
        const auto data = payload.substr(offset);
        dictionary_page_ = encoding == 2 || encoding == 8;
        if (encoding == 0) { values_ = parquet_detail::plain(data,column_,present); }
        else if (dictionary_page_) {
          if (present == 0) { indices_.clear(); }
          else {
            if (data.empty() || dictionary_.empty()) { throw std::runtime_error("missing Parquet dictionary"); }
            indices_ = parquet_detail::hybrid(data.substr(1),static_cast<unsigned char>(data[0]),present);
          }
        } else { throw std::runtime_error("unsupported Parquet value encoding"); }
        if (count_ != 0) { return; }
      }
      throw std::runtime_error("Parquet page count limit exceeded");
    }
    SecureInputStream input_;
    parquet_detail::Column column_;
    parquet_detail::Chunk chunk_;
    std::size_t &memory_;
    std::size_t charge_ = 0, dictionary_charge_ = 0;
    std::int64_t remaining_ = 0;
    std::size_t count_ = 0, position_ = 0, value_position_ = 0;
    bool dictionary_page_ = false;
    std::vector<CsvField> dictionary_, values_;
    std::vector<unsigned> indices_, definitions_;
  };
public:
  ParquetReader(const std::filesystem::path &path, const std::filesystem::path &root,
                const std::vector<std::size_t> &selected)
  {
    SecureInputStream input(path,root);
    metadata_ = parquet_detail::metadata(input);
    if (selected.size() > 256) { throw std::runtime_error("Parquet projected column limit exceeded"); }
    for (const auto index : selected) {
      if (index >= metadata_.columns.size()) { throw std::runtime_error("invalid Parquet projected column"); }
      selected_.push_back(index);
      columns_.push_back(std::make_unique<ColumnReader>(path,root,metadata_.columns[index],memory_));
    }
  }
  const CsvSchema &schema() const { return metadata_.schema; }
  bool next(std::vector<CsvField> &row)
  {
    while (group_remaining_ == 0) {
      if (group_index_ == metadata_.groups.size()) { return false; }
      const auto &group = metadata_.groups[group_index_++];
      group_remaining_ = group.rows;
      if (group_remaining_ != 0) {
        for (std::size_t i = 0; i < columns_.size(); ++i) { columns_[i]->begin(group.chunks[selected_[i]],group.rows); }
      }
    }
    row.clear(); row.reserve(columns_.size());
    for (auto &column : columns_) { row.push_back(column->next()); }
    --group_remaining_;
    return true;
  }
private:
  parquet_detail::Metadata metadata_;
  std::vector<std::size_t> selected_;
  std::size_t group_index_ = 0, memory_ = 0;
  std::int64_t group_remaining_ = 0;
  std::vector<std::unique_ptr<ColumnReader>> columns_;
};
} // namespace oceanbase::sql::file_sql

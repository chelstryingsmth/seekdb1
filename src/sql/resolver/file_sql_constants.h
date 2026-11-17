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
#include "sql/resolver/ob_resolver_define.h"
#include "sql/parser/parse_node.h"
#include <cstring>
#include <stdexcept>
#include <new>
#include <cstdint>

namespace oceanbase::sql::file_sql {
// Automatic SQL parameterization may replace even a filename literal with '?'.
// Resolve that bound constant at inference time; never cache its inferred schema.
inline const ParseNode *bound_constant(ObResolverParams &params, const ParseNode *node)
{
  if (node == nullptr) { throw std::invalid_argument("missing file option value"); }
  if (node->type_ != T_QUESTIONMARK && node->type_ != T_OP_NEG) { return node; }
  ParseNode value{};
  if (node->type_ == T_OP_NEG) {
    if (node->num_child_ != 1) { throw std::invalid_argument("invalid negative file option"); }
    const ParseNode *child = bound_constant(params,node->children_[0]);
    if (child->type_ != T_INT || child->value_ == INT64_MIN) { throw std::invalid_argument("invalid negative file option"); }
    value.type_ = T_INT; value.value_ = -child->value_;
  } else {
    if (params.param_list_ == nullptr || node->value_ < 0 || node->value_ >= params.param_list_->count()) {
      throw std::invalid_argument("file paths require bound values during schema inference");
    }
    const auto &parameter = params.param_list_->at(node->value_);
    if (parameter.is_null()) { value.type_ = T_NULL; }
    else if (common::ob_is_string_type(parameter.get_type())) {
      const auto text = parameter.get_string();
      value.type_ = T_VARCHAR; value.str_value_ = text.ptr(); value.str_len_ = text.length();
    } else if (parameter.get_type() == common::ObIntType) {
      value.type_ = T_INT; value.value_ = parameter.get_int();
    } else if (parameter.get_type() == common::ObTinyIntType) {
      value.type_ = T_BOOL; value.value_ = parameter.get_tinyint();
    } else { throw std::invalid_argument("unsupported bound file option type"); }
  }
  if (params.allocator_ == nullptr) { throw std::invalid_argument("missing file binding allocator"); }
  auto *copy = static_cast<ParseNode *>(params.allocator_->alloc(sizeof(ParseNode)));
  if (copy == nullptr) { throw std::bad_alloc(); }
  std::memcpy(copy,&value,sizeof(value));
  return copy;
}
} // namespace oceanbase::sql::file_sql

#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <string_view>

namespace JsonRead {
  inline std::string_view str(const rapidjson::Value& v, const char* key) {
    if(!v.IsObject())
      return {};
    auto it = v.FindMember(key);
    if(it==v.MemberEnd() || !it->value.IsString())
      return {};
    return std::string_view(it->value.GetString(),it->value.GetStringLength());
    }

  inline bool boolean(const rapidjson::Value& v, const char* key, bool def) {
    if(!v.IsObject())
      return def;
    auto it = v.FindMember(key);
    if(it==v.MemberEnd() || !it->value.IsBool())
      return def;
    return it->value.GetBool();
    }

  inline int64_t integer(const rapidjson::Value& v, const char* key, int64_t def) {
    if(!v.IsObject())
      return def;
    auto it = v.FindMember(key);
    if(it==v.MemberEnd() || !it->value.IsNumber())
      return def;
    if(it->value.IsInt64())
      return it->value.GetInt64();
    return int64_t(it->value.GetDouble());
    }
  }

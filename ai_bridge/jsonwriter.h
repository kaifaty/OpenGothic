#pragma once

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <string>
#include <string_view>

// Builds one protocol message: {"type":<type>, ...}
class JsonWriter final {
  public:
    explicit JsonWriter(std::string_view type):wr(buf) {
      wr.StartObject();
      str("type",type);
      }

    JsonWriter& str(std::string_view key, std::string_view v) { name(key); string(v); return *this; }
    JsonWriter& num(std::string_view key, double v)           { name(key); wr.Double(v); return *this; }
    JsonWriter& integer(std::string_view key, int64_t v)      { name(key); wr.Int64(v); return *this; }
    JsonWriter& boolean(std::string_view key, bool v)         { name(key); wr.Bool(v); return *this; }
    JsonWriter& null(std::string_view key)                    { name(key); wr.Null(); return *this; }

    JsonWriter& object(std::string_view key) { name(key); wr.StartObject(); return *this; }
    JsonWriter& array (std::string_view key) { name(key); wr.StartArray();  return *this; }
    JsonWriter& object()                     { wr.StartObject(); return *this; }
    JsonWriter& end()                        { endObj(); return *this; }
    JsonWriter& endArray()                   { wr.EndArray(); return *this; }

    JsonWriter& item(std::string_view v)     { string(v); return *this; }
    JsonWriter& item(double v)               { wr.Double(v); return *this; }

    std::string finish() {
      wr.EndObject();
      return std::string(buf.GetString(),buf.GetSize());
      }

  private:
    void name(std::string_view key) { wr.Key(key.data(),rapidjson::SizeType(key.size())); }
    void string(std::string_view v) { wr.String(v.data(),rapidjson::SizeType(v.size())); }
    void endObj()                   { wr.EndObject(); }

    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> wr;
  };

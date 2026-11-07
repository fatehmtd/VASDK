#pragma once

// Shared by the C API's sources, not part of it: what a tool or an extractor callback fills, and
// protos in and out in the format the caller asked for.

#include "c/vasdk.h"

#include <cstddef>
#include <string>
#include <vector>

namespace google::protobuf { class Message; }

struct vasdk_result {
    std::string json;
    std::vector<float> floats;   // vasdk_result_set_floats
    bool hasFloats = false;
    std::string error;
    bool failed = false;
    bool set = false;            // vasdk_result_set or _set_floats was called
};

namespace vasdk_c {

/// `message` in `format`: serialized, JSON or text.
std::string written(const google::protobuf::Message& message, vasdk_format format);
/// `message` in `format`, malloc'd for the caller (vasdk_free), *length its size. Never NULL.
void* writtenCopy(const google::protobuf::Message& message, vasdk_format format, size_t* length);
/// Parses `length` bytes in `format` into `message`; false, with `error` set, when they don't parse.
/// NULL data is an empty message.
bool read(const void* data, size_t length, vasdk_format format, google::protobuf::Message& message, std::string* error);

} // namespace vasdk_c

#pragma once

// Shared by the C API's sources, not part of it: what a tool or an extractor callback fills, and
// protos in and out in the format the caller asked for.

#include "c/vasdk.h"

#include <cstddef>
#include <string>

namespace google::protobuf { class Message; }

struct vasdk_result {
    std::string json;
    std::string error;
    bool failed = false;
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

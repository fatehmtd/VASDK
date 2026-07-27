#pragma once

#include <VASDK/llm/PromptTemplate.h>
#include <jni.h>
#include <optional>
#include <string>

std::string jstringToStdString(JNIEnv *env, jstring jstr);

// Map a caller-provided chat-format name to a ChatFormat enum value.
// The formatted prompt itself always comes from the GGUF's embedded template;
// this only selects how auxiliary prompt text (tool descriptions, call syntax)
// is rendered, so it must match the loaded model family.
std::optional<va::llm::ChatFormat> chatFormatFromString(std::string name);

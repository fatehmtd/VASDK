#pragma once

#include <VASDK/llm/PromptTemplate.h>
#include <jni.h>
#include <optional>
#include <string>

std::string jstringToStdString(JNIEnv *env, jstring jstr);

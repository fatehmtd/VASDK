#include "JniUtils.h"

#include <algorithm>
#include <cctype>

std::string jstringToStdString(JNIEnv *env, jstring jstr) {
    const char *cstr = env->GetStringUTFChars(jstr, nullptr);
    std::string str(cstr);
    env->ReleaseStringUTFChars(jstr, cstr);
    return str;
}

std::optional<va::llm::ChatFormat> chatFormatFromString(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "openai") return va::llm::ChatFormat::OPENAI;
    if (name == "anthropic") return va::llm::ChatFormat::ANTHROPIC;
    if (name == "google") return va::llm::ChatFormat::GOOGLE;
    if (name == "chatml") return va::llm::ChatFormat::CHATML;
    if (name == "llama3") return va::llm::ChatFormat::LLAMA3;
    if (name == "llama2") return va::llm::ChatFormat::LLAMA2;
    if (name == "mistral") return va::llm::ChatFormat::MISTRAL;
    if (name == "gemma") return va::llm::ChatFormat::GEMMA;
    if (name == "phi3") return va::llm::ChatFormat::PHI3;
    if (name == "lfm2") return va::llm::ChatFormat::LFM2;
    if (name == "lfm2.5" || name == "lfm2_5") return va::llm::ChatFormat::LFM2_5;
    return std::nullopt;
}

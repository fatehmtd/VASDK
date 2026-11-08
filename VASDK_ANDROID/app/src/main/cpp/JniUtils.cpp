#include "JniUtils.h"

#include <algorithm>
#include <cctype>

std::string jstringToStdString(JNIEnv *env, jstring jstr) {
    const char *cstr = env->GetStringUTFChars(jstr, nullptr);
    std::string str(cstr);
    env->ReleaseStringUTFChars(jstr, cstr);
    return str;
}

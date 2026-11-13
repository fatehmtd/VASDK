// The main of every VASDK test program (vasdk_add_gtest): GoogleTest and gMock's flags, then
// VASDK's own, then every test. --vasdk-models-dir=<folder> becomes VASDK_TEST_MODELS_DIR, which
// va::test::modelsDirectory() reads, unless that variable is already set: then it wins, so a run
// can point at other models.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

void setVariableUnlessSet(const char* name, const std::string& value) {
    if (std::getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 0);
#endif
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleMock(&argc, argv);   // takes out the flags it knows
    const std::string modelsDirectoryFlag = "--vasdk-models-dir=";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind(modelsDirectoryFlag, 0) == 0)
            setVariableUnlessSet("VASDK_TEST_MODELS_DIR", argument.substr(modelsDirectoryFlag.size()));
    }
    return RUN_ALL_TESTS();
}

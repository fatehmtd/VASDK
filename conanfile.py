"""Conan 2 recipe: the C library vasdk_c (VASDK::c) built from this source tree (docs/integrating.md, "Conan").

    conan create . -s compiler.cppstd=17 --build=missing                # flavor default
    conan create . -s compiler.cppstd=17 --build=missing -o "vasdk/*:flavor=full"

A consumer then requires vasdk/<version> and uses VASDK's own find_package(VASDK) and VASDK::c,
the same as from a release archive (test_package/ is one).

Libraries come from Conan: spdlog, nlohmann_json, cpp-httplib, protobuf, libcurl, highway. They are
linked into vasdk_c and hidden, so a consumer links nothing else. The rest of what the build needs
is downloaded by the build itself, pinned (VASDK_DEPENDENCY_SOURCE AUTO): ONNX Runtime (Microsoft's
prebuilt shared library, installed beside vasdk_c) and the sources a few providers compile in.
Models are never packaged: the model store fetches them at run time (docs/models.md).
"""

import os
import shutil

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy, load
from conan.tools.scm import Git

required_conan_version = ">=2.0.9"

# What the C library's build reads from the repository: the rest (the sample apps, the bindings,
# the docs) is left out of the package's sources.
SOURCE_PATHS = ("CMakeLists.txt", "VERSION", "LICENSE", "THIRD_PARTY_NOTICES.md", "cmake", "VASDK", "VASDK_3P_IMPL")


class VasdkConan(ConanFile):
    name = "vasdk"
    description = "Voice assistant SDK: VAD, speech-to-text, turn detection, LLM and text-to-speech providers behind one C API"
    license = "Apache-2.0"
    url = "https://github.com/fatehmtd/VASDK"
    homepage = "https://github.com/fatehmtd/VASDK"
    topics = ("voice-assistant", "speech-recognition", "text-to-speech", "vad", "onnx", "c-api")
    package_type = "shared-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        # The providers built into the library (cmake/VASDKFlavors.cmake)
        "flavor": ["core", "default", "full"],
    }
    default_options = {
        "flavor": "default",
    }

    def set_version(self):
        self.version = self.version or load(self, os.path.join(self.recipe_folder, "VERSION")).strip()

    def export_sources(self):
        # The files git tracks, so nothing a local build left in the tree (models, build output)
        # goes into the package; from a source archive, every file under SOURCE_PATHS.
        try:
            files = Git(self, self.recipe_folder).included_files()
        except Exception:
            files = None
        if files is None:
            for path in SOURCE_PATHS:
                source = os.path.join(self.recipe_folder, path)
                if os.path.isdir(source):
                    copy(self, "*", source, os.path.join(self.export_sources_folder, path))
                else:
                    copy(self, path, self.recipe_folder, self.export_sources_folder)
            return
        for file in files:
            if file.split("/")[0] not in SOURCE_PATHS:
                continue
            destination = os.path.join(self.export_sources_folder, file)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            shutil.copy2(os.path.join(self.recipe_folder, file), destination)

    def configure(self):
        if self.settings.os == "Windows":
            # The OS TLS stack, as VASDK's own curl build uses: no OpenSSL to build
            self.options["libcurl"].with_ssl = "schannel"

    def layout(self):
        # Not build/: that is where a plain CMake build of this tree goes
        cmake_layout(self, build_folder="build-conan")

    def requirements(self):
        # The versions VASDK fetches itself (cmake/VASDKDependencies.cmake, VASDK/cmake/)
        self.requires("spdlog/1.15.0")
        self.requires("nlohmann_json/3.11.3")
        self.requires("cpp-httplib/0.18.3")
        self.requires("protobuf/7.36.2")   # protobuf 36.2
        self.requires("libcurl/8.21.0")
        self.requires("highway/1.2.0")

    def validate(self):
        check_min_cppstd(self, 17)

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.cache_variables["VASDK_FLAVOR"] = str(self.options.flavor)
        toolchain.cache_variables["VASDK_DEPENDENCY_SOURCE"] = "AUTO"
        toolchain.cache_variables["VASDK_BUILD_C_LIBRARY"] = True
        toolchain.cache_variables["VASDK_BUILD_TESTS"] = False
        toolchain.cache_variables["VASDK_BUILD_APPS"] = False
        toolchain.cache_variables["VASDK_DOWNLOAD_MODELS"] = False
        # Conan delivers the dependencies' own DLLs, when they are shared
        toolchain.cache_variables["VASDK_INSTALL_RUNTIME_DEPENDENCIES"] = False
        toolchain.generate()
        deps = CMakeDeps(self)
        # Highway under the names of its own package file, which VASDK looks for: hwy, hwy::hwy
        deps.set_property("highway", "cmake_file_name", "hwy")
        deps.set_property("highway::hwy", "cmake_target_name", "hwy::hwy")
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()

    def package_info(self):
        # VASDK's own package file, installed with the library: find_package(VASDK) and VASDK::c,
        # its components (the providers built in) and vasdk_copy_runtime()
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = ["."]
        # Other build systems (PkgConfigDeps, MSBuildDeps...)
        self.cpp_info.set_property("pkg_config_name", "vasdk")
        self.cpp_info.libs = ["vasdk_c"]

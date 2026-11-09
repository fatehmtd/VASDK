"""conan create's check of the package: a C program and a C++ one, built against vasdk with
find_package(VASDK) and run."""

import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, cmake_layout


class VasdkTestConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeToolchain", "VirtualRunEnv"
    test_type = "explicit"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            for program in ("test_package", "test_package_cpp"):
                self.run(os.path.join(self.cpp.build.bindir, program), env="conanrun")

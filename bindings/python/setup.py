"""The wheel holds a native library (VASDK, loaded with ctypes) but no Python extension: it's
for one platform, and any Python 3 on it. Metadata is in pyproject.toml."""

from setuptools import setup
from setuptools.command.bdist_wheel import bdist_wheel


class PlatformWheel(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False   # the platform's library is inside

    def get_tag(self):
        _, _, platform = super().get_tag()
        return "py3", "none", platform


setup(cmdclass={"bdist_wheel": PlatformWheel})

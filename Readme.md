# Boomerang Decompiler

This is a fork of the [Boomerang Decompiler](http://boomerang.sourceforge.net/), a general, open source (BSD licensed) machine code decompiler.
Right now, there are no pre-compiled binaries available, so you'll have to compile from source yourself.


## Building

| **Build status** | Linux | Windows | Test Coverage |
|------------------|-------|---------|---------------|
|    **develop**   | [![Travis CI](https://api.travis-ci.com/BoomerangDecompiler/boomerang.svg?branch=develop)](https://travis-ci.com/BoomerangDecompiler/boomerang) | [![Appveyor](https://ci.appveyor.com/api/projects/status/9ftcn56cys23784j/branch/develop?svg=true)](https://ci.appveyor.com/project/nemerle/boomerang/branch/develop) | [![codecov](https://codecov.io/gh/BoomerangDecompiler/boomerang/branch/develop/graph/badge.svg)](https://codecov.io/gh/BoomerangDecompiler/boomerang/branch/develop) |
|    **master**    | [![Travis CI](https://api.travis-ci.com/BoomerangDecompiler/boomerang.svg?branch=master)](https://travis-ci.com/BoomerangDecompiler/boomerang)  | [![Appveyor](https://ci.appveyor.com/api/projects/status/9ftcn56cys23784j/branch/master?svg=true)](https://ci.appveyor.com/project/nemerle/boomerang/branch/master)   | [![codecov](https://codecov.io/gh/BoomerangDecompiler/boomerang/branch/master/graph/badge.svg)](https://codecov.io/gh/BoomerangDecompiler/boomerang/branch/master)   |


### Building prerequisites

 - A 64 bit operating system (32 bit might or might not work, but it is not supported.)
 - A C++ compiler compatible with C++17 (GCC 7+, Clang 5+, MSVC 2017+ are known to work)
 - [CMake 3.8.0 or newer](https://cmake.org/download/)
 - [Qt5](https://www.qt.io/download-open-source/) (Qt 5.9+ is known to work, earlier versions should also work)
 - [Doxygen 1.8.13 or newer](http://www.doxygen.nl/) (optional, for documentation)
 - [Python 3](https://www.python.org/downloads/) (optional, for regression tests)


### Building on Linux

On a Linux system you can build and install Boomerang with the usual cmake-make-make-install procedure.
On a Debian-compatible system (e.g. Ubuntu) these commands will clone, build and install Boomerang:

```bash
sudo apt-get install git build-essential cmake qt5-default
cd YOUR_FAVOURITE_DEVELOPMENT_DIRECTORY
git clone https://github.com/BoomerangDecompiler/boomerang.git
cd boomerang && mkdir build && cd build
cmake .. && make && make install
```


### Building on Windows

To compile on Windows, it should be enough to run cmake-gui and fill in the paths to missing libraries if CMake does not find them.


### Building on macOS

Building on macOS is currently not supported. However, patches or pull requests on this matter are welcome. See also issue #39.


## Testing

### Unit tests

Boomerang has a unit test suite, which can be run by `make && make test` on Linux or by running the RUN_TESTS target in Visual Studio.
Make sure you have the BOOMERANG_BUILD_UNIT_TESTS option set in CMake.


### Regression tests

Additionally, you can run the regression test suite, to do so you will need a Python 3 interpeter.
To run the regression test suite, make sure the BOOMERANG_BUILD_REGRESSION_TESTS option is set in CMake, then run `make check`
on Linux. Building the regression test suite on Windows is currently not supported.

When the regression test suite finds a regression in the output, it is shown as a unified diff.
If you have not modified Boomerang, please file the regression(s) as a bug report at https://github.com/BoomerangDecompiler/boomerang/issues.


# Contributing

Boomerang uses the [gitflow workflow](https://nvie.com/posts/a-successful-git-branching-model/). If you want to fix a bug or implement a small enhancement,
please branch off from the `develop` branch (`git checkout -b`) and submit your fix or enhancement as a pull request to the `develop` branch.
If you want to implement a larger feature, please open an issue about the new feature on the issue tracker first, so the feature can be discussed first.
For additional information, please read the [contributing guidelines](https://github.com/BoomerangDecompiler/boomerang/blob/develop/Contributing.md).

Thanks for your interest in the Boomerang Decompiler!


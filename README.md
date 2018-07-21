# WebServer

This is a stand-alone program which runs an HTTP server.  It creates an instance of the `Http::Server` class and provides it with an instance of the `HttpNetworkTransport::HttpServerNetworkTransport` class in order to run a web server which is connected to the network of the host machine and serves any incoming HTTP requests on the port that it binds.

## Usage

Currently this program does not use or require any command-line parameters.  It is hard-coded to bind and listen to port 8080 for any TCP connections, accept any incoming connections, parse any messages received on connections as HTTP requests, and send back HTTP responses.

Currently, all valid HTTP requests are answered with 404 Not Found HTTP responses.  All invalid HTTP requests are answered with 400 Bad Request HTTP responses.

## Supported platforms / recommended toolchains

This is a portable C++11 library which depends only on the C++11 compiler and standard library, so it should be supported on almost any platform.  The following are recommended toolchains for popular platforms.

* Windows -- [Visual Studio](https://www.visualstudio.com/) (Microsoft Visual C++)
* Linux -- clang or gcc
* MacOS -- Xcode (clang)

## Building

This library is not intended to stand alone.  It is intended to be included in a larger solution which uses [CMake](https://cmake.org/) to generate the build system and build applications which will link with the library.

There are two distinct steps in the build process:

1. Generation of the build system, using CMake
2. Compiling, linking, etc., using CMake-compatible toolchain

### Prerequisites

* [CMake](https://cmake.org/) version 3.8 or newer
* C++11 toolchain compatible with CMake for your development platform (e.g. [Visual Studio](https://www.visualstudio.com/) on Windows)

### Build system generation

Generate the build system using [CMake](https://cmake.org/) from the solution root.  For example:

```bash
mkdir build
cd build
cmake -G "Visual Studio 15 2017" -A "x64" ..
```

### Compiling, linking, et cetera

Either use [CMake](https://cmake.org/) or your toolchain's IDE to build.
For [CMake](https://cmake.org/):

```bash
cd build
cmake --build . --config Release
```

# WebServer

This is a stand-alone program which runs an HTTP server.  Through plug-in
modules, the server may provide resources to clients upon request.

## Usage

    Usage: WebServer [-c <CONFIG>]

    Run an HTTP server.

      CONFIG  Optional path to a configuration file

The `WebServer` program provides a basic framework for providing resources over
the HTTP protocol (optionally on top of TLS/SSL).  Actual resources are
provided by plug-ins to the `WebServer` program.  The program and its plug-ins
are configured by a configuration file formatted in JSON (JavaScript Object
Notation).

Unless a path to the configuration file is provided, `WebServer` attempts to
locate a configuration file by looking for files named `config.json`, first in
the current working directory of the program, and then in the directory
containing the program's image file.

### Configuration File

The following is an example configuration file where the server is configured
to accept HTTP over SSL ("https" scheme) requests (`secure` set to `true`,
the path to the server's certificate provided by `sslCertificate`, the
path to the server's private key provided by `sslKey`, and the passphrase
to that private key provided by `sslKeyPassphrase`).

In this example, three plug-ins are configured.  The source code to these
plugins are included in this repository.

* ChatRoomPlugin -- This implements an example "chat room" accessible via
  WebSocket.
* EchoPlugin -- This serves a dynamic resource which consists of an HTML page
  containing echoed information about the resource request itself.
* StaticContentPlugin -- This serves static files available on the filesystem
  as resources.

```json
{
    "server": {
        "HeaderLineLimit": "1000",
        "Port": "8080",
        "InactivityTimeout": "1.0",
        "RequestTimeout": "60.0",
        "IdleTimeout": "60.0",
        "BadRequestReportBytes": "100",
        "InitialBanPeriod": "60.0",
        "ProbationPeriod": "60.0",
        "TooManyRequestsThreshold": "10.0",
        "TooManyRequestsMeasurementPeriod": "1.0"
    },
    "secure": true,
    "sslCertificate": "cert.pem",
    "sslKey": "key.pem",
    "sslKeyPassphrase": "password",
    "plugins-image": "plugins",
    "plugins-runtime": "plugins/runtime",
    "plugins-enabled": ["ChatRoomPlugin", "EchoPlugin", "StaticContentPlugin"],
    "plugins": {
        "ChatRoomPlugin": {
            "module": "ChatRoomPlugin",
            "configuration": {
                "space": "/chat",
                "tellTimeout": 1.0,
                "mathQuiz": {
                    "minCoolDown": 10.0,
                    "maxCoolDown": 30.0
                },
                "nicknames": [
                    "Alice",
                    "Bob",
                    "Carol"
                ]
            }
        },
        "EchoPlugin": {
            "module": "EchoPlugin",
            "configuration": {
                "space": "/echo"
            }
        },
        "StaticContentPlugin": {
            "module": "StaticContentPlugin",
            "configuration": {
                "spaces": [
                    {
                        "space": "/",
                        "root": "../../TestStaticContent"
                    },
                    {
                        "space": "/chatter",
                        "root": "../../chatter/build"
                    }
                ],
                "filter": {
                    "bar": ["*.html", "*.css"]
                }
            }
        }
    }
}
```

## Supported platforms / recommended toolchains

This is a portable C++11 application which depends only on the C++11 compiler,
the C and C++ standard libraries, and other C++11 libraries with similar
dependencies, so it should be supported on almost any platform.  The following
are recommended toolchains for popular platforms.

* Windows -- [Visual Studio](https://www.visualstudio.com/) (Microsoft Visual
  C++)
* Linux -- clang or gcc
* MacOS -- Xcode (clang)

## Building

This application is not intended to stand alone.  It is intended to be included
in a larger solution which uses [CMake](https://cmake.org/) to generate the
build system and provide the application with its dependencies.

There are two distinct steps in the build process:

1. Generation of the build system, using CMake
2. Compiling, linking, etc., using CMake-compatible toolchain

### Prerequisites

* [CMake](https://cmake.org/) version 3.8 or newer
* C++11 toolchain compatible with CMake for your development platform (e.g.
  [Visual Studio](https://www.visualstudio.com/) on Windows)
* [Http](https://github.com/rhymu8354/Http.git) - a library which implements
  [RFC 7230](https://tools.ietf.org/html/rfc7230), "Hypertext Transfer Protocol
  (HTTP/1.1): Message Syntax and Routing".
* [HttpNetworkTransport](https://github.com/rhymu8354/HttpNetworkTransport.git) -
  a library which implements the transport interfaces needed by the `Http`
  library, in terms of the network endpoint and connection abstractions
  provided by the `SystemAbstractions` library.
* [Json](https://github.com/rhymu8354/Json.git) - a library which implements
  [RFC 7159](https://tools.ietf.org/html/rfc7159), "The JavaScript Object
  Notation (JSON) Data Interchange Format".
* [StringExtensions](https://github.com/rhymu8354/StringExtensions.git) - a
  library containing C++ string-oriented libraries, many of which ought to be
  in the standard library, but aren't.
* [SystemAbstractions](https://github.com/rhymu8354/SystemAbstractions.git) - a
  cross-platform adapter library for system services whose APIs vary from one
  operating system to another
* [TlsDecorator](https://github.com/rhymu8354/TlsDecorator.git) - an adapter to
  use `LibreSSL` to encrypt traffic passing through a network connection
  provided by `SystemAbstractions`

### Build system generation

Generate the build system using [CMake](https://cmake.org/) from the solution
root.  For example:

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

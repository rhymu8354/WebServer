/**
 * @file main.cpp
 *
 * This module holds the main() function, which is the entrypoint
 * to the program.
 *
 * © 2018 by Richard Walters
 */

#include <chrono>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <Http/Server.hpp>
#include <HttpNetworkTransport/HttpServerNetworkTransport.hpp>
#include <SystemAbstractions/DiagnosticsStreamReporter.hpp>
#include <thread>

namespace {

    /**
     * This flag indicates whether or not the web server should shut down.
     */
    bool shutDown = false;

}

/**
 * This function is set up to be called when the SIGINT signal is
 * received by the program.  It just sets the "shutDown" flag
 * and relies on the program to be polling the flag to detect
 * when it's been set.
 *
 * @param[in] sig
 *     This is the signal for which this function was called.
 */
void InterruptHandler(int) {
    shutDown = true;
}

/**
 * This function is the entrypoint of the program.
 * It just sets up the web server and then waits for
 * the SIGINT signal to know when the web server should
 * be shut down and program terminated.
 *
 * @param[in] argc
 *     This is the number of command-line arguments given to the program.
 *
 * @param[in] argv
 *     This is the array of command-line arguments given to the program.
 */
int main(int argc, char* argv[]) {
#ifdef _WIN32
    //_crtBreakAlloc = 18;
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif /* _WIN32 */
    auto transport = std::make_shared< HttpNetworkTransport::HttpServerNetworkTransport >();
    Http::Server server;
    const auto diagnosticsSubscription = server.SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsStreamReporter(stdout, stderr)
    );
    if (!server.Mobilize(transport, 8080)) {
        return EXIT_FAILURE;
    }
    const auto previousInterruptHandler = signal(SIGINT, InterruptHandler);
    printf("Web server up and running.\n");
    while (!shutDown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    (void)signal(SIGINT, previousInterruptHandler);
    printf("Exiting...\n");
    return EXIT_SUCCESS;
}

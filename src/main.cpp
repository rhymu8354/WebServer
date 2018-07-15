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
#include <thread>

namespace {

    /**
     * This flag indicates whether or not the web server should shut down.
     */
    bool shutDown = false;

}

void InterruptHandler(int sig) {
    shutDown = true;
}

int main(int argc, char* argv[]) {
    Http::Server server;
    const auto previousInterruptHandler = signal(SIGINT, InterruptHandler);
    printf("Web server up and running.\n");
    while (!shutDown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    (void)signal(SIGINT, previousInterruptHandler);
    printf("Exiting...\n");
    return EXIT_SUCCESS;
}

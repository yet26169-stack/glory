#include "core/Log.h"
#include "core/Application.h"

#include <cstdlib>
#include <stdexcept>

int main() {
    glory::Log::init();

    try {
        glory::Application app("Glory Engine", 1280, 720);
        app.run();
    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

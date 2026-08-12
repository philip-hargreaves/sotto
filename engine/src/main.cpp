#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_server.hpp"

int main(int argc, char* argv[]) {
    try {
        // Tests pass a private pipe name so runs cannot collide with the app
        std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\sotto-engine";
        if (argc > 1) {
            pipe_name = L"\\\\.\\pipe\\" + std::wstring(argv[1], argv[1] + std::strlen(argv[1]));
        }

        sotto::ipc::PipeServer server(pipe_name);
        sotto::ipc::RegisterMethods(server);
        server.ServeOneClient();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sotto-engine: %s\n", e.what());
        return 1;
    }
}

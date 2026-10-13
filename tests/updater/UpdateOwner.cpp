// Disposable process fixture for the installer; never linked into the player.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--wait") {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!std::filesystem::exists(argv[2])) {
            if (std::chrono::steady_clock::now() > deadline) return 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } else if (argc >= 3 && std::string(argv[1]) == "--restarted") {
        std::ofstream result(argv[2]);
        for (int i = 3; i < argc; ++i) result << argv[i] << '\n';
    }
    return 0;
}

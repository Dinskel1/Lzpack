#include <clocale>
#include <exception>
#include <new>
#include <string>
#include <vector>
#include "cli.h"
#include "display.h"
#include "errors.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::string> collectArguments(int argc, char** argv) {
    std::vector<std::string> arguments;
#ifdef _WIN32
    int wideCount = 0;
    LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (wideArguments != nullptr) {
        for (int i = 0; i < wideCount; ++i) {
            const int length = WideCharToMultiByte(CP_UTF8, 0, wideArguments[i], -1, nullptr, 0, nullptr, nullptr);
            if (length <= 1) {
                arguments.emplace_back();
                continue;
            }

            std::string text(static_cast<std::size_t>(length), '\0');
            if (WideCharToMultiByte(CP_UTF8, 0, wideArguments[i], -1, &text[0], length, nullptr, nullptr) == 0) {
                arguments.emplace_back();
                continue;
            }
            text.resize(static_cast<std::size_t>(length - 1));
            arguments.push_back(std::move(text));
        }
        LocalFree(wideArguments);
    }
#endif
    if (arguments.empty()) {
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i] == nullptr ? "" : argv[i]);
    }
    return arguments;
}

}

int main(int argc, char** argv) {

    std::setlocale(LC_ALL, "");

    std::vector<std::string> arguments = collectArguments(argc, argv);

    if (!arguments.empty()) {
        arguments.erase(arguments.begin());
    }

    try {
        const lzpack::ExitCode code = lzpack::cli::run(arguments);
        return static_cast<int>(code);
    } catch (const lzpack::CompressError& error) {
        lzpack::display::error(error.what());
        return static_cast<int>(error.exitCode());
    } catch (const std::bad_alloc&) {
        lzpack::display::error("out of memory");
        return static_cast<int>(lzpack::ExitCode::Internal);
    } catch (const std::exception& error) {
        lzpack::display::error(std::string("unknown error:") + error.what());
        return static_cast<int>(lzpack::ExitCode::Internal);
    } catch (...) {
        lzpack::display::error("unknown error");
        return static_cast<int>(lzpack::ExitCode::Internal);
    }
}

#pragma once

#include <string>
#include <vector>

#include "errors.h"

namespace lzpack {
namespace cli {

ExitCode run(const std::vector<std::string>& arguments);

void printHelp();

bool printCommandHelp(const std::string& command);

const std::vector<std::string>& commandNames();

}
}

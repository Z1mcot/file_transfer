#pragma once

#include <filesystem>

namespace file_transfer::application {

[[nodiscard]] std::filesystem::path executable_directory();

} // namespace file_transfer::application
#pragma once

#include "file_transfer/storage/i_staged_file.hpp"

#include <memory>

namespace file_transfer {

class IFileStore {
public:
    virtual ~IFileStore() = default;
    [[nodiscard]] virtual std::unique_ptr<IStagedFile> create_staged_file() = 0;
};

} // namespace file_transfer
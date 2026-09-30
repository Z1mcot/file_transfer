#pragma once

#include "file_transfer/cli.hpp"

namespace file_transfer {

class ClientApplication final {
public:
    explicit ClientApplication(ClientOptions options);
    void run();

private:
    ClientOptions options_;
};

} // namespace file_transfer
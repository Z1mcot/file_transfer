#pragma once

#include "file_transfer/storage/i_file_store.hpp"
#include "file_transfer/transport/i_transport_listener.hpp"

#include <memory>
#include <string>

namespace file_transfer::application {

class ServerWorkers final {
public:
    ServerWorkers(ITransportListener& listener, std::shared_ptr<IFileStore> store);
    ~ServerWorkers();
    ServerWorkers(const ServerWorkers&) = delete;
    ServerWorkers& operator=(const ServerWorkers&) = delete;

    void reap_completed();
    [[nodiscard]] bool try_start(AcceptedConnection connection);
    void shutdown() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace file_transfer::application
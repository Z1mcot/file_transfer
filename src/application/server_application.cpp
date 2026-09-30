#include "file_transfer/application/server_application.hpp"

#include "file_transfer/application/executable_path.hpp"
#include "file_transfer/application/logging.hpp"
#include "file_transfer/application/server_workers.hpp"
#include "file_transfer/application/signal_waiter.hpp"
#include "file_transfer/storage/file_store.hpp"
#include "file_transfer/transport/tcp_listener.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace file_transfer {

ServerApplication::ServerApplication(std::uint16_t port, std::filesystem::path output_directory)
    : port_(port), output_directory_(std::move(output_directory)) {}

void ServerApplication::run() {
    TcpListener listener(port_);
    auto store = std::make_shared<FileStore>(output_directory_);
    application::log_parts("[SERVER] Listening on ", listener.local_endpoint());

    std::atomic<bool> stopping{false};
    application::SignalWaiter signal_waiter(listener, stopping);
    application::ServerWorkers workers(listener, std::move(store));
    std::chrono::milliseconds accept_retry_delay{100};

    while (!stopping.load(std::memory_order_acquire)) {
        workers.reap_completed();
        std::optional<AcceptedConnection> incoming;
        try {
            incoming = listener.accept();
        } catch (const std::system_error& error) {
            if ((error.code().value() == ECANCELED || error.code().value() == EINTR) &&
                stopping.load(std::memory_order_acquire)) {
                break;
            }
            if (error.code().value() == EINTR) {
                continue;
            }
            if (stopping.load(std::memory_order_acquire)) {
                break;
            }
            application::log_parts("[SERVER] Accept failed: ", error.what());
            const int code = error.code().value();
            if (code == EMFILE || code == ENFILE || code == ENOBUFS || code == ENOMEM) {
                std::this_thread::sleep_for(accept_retry_delay);
                accept_retry_delay = std::min(accept_retry_delay * 2, std::chrono::milliseconds{2000});
            }
            continue;
        }
        if (!incoming) {
            continue;
        }
        accept_retry_delay = std::chrono::milliseconds{100};
        application::log_parts("[SERVER] Client connected: ", incoming->peer);
        (void)workers.try_start(std::move(*incoming));
    }

    workers.shutdown();
    application::log_line("[SERVER] Stopped");
}

} // namespace file_transfer
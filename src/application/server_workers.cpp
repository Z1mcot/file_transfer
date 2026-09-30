#include "server_workers.hpp"

#include "logging.hpp"
#include "file_transfer/transfer.hpp"

#include <algorithm>
#include <atomic>
#include <list>
#include <thread>
#include <utility>
#include <vector>

namespace file_transfer::application {
namespace {

constexpr std::size_t maximum_active_clients = 32U;

struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<ITransport> transport;
    std::string peer;
};

} // namespace

class ServerWorkers::Implementation final {
public:
    Implementation(ITransportListener& listener, std::shared_ptr<IFileStore> store)
        : listener_(listener), store_(std::move(store)) {}

    ~Implementation() { shutdown(); }

    void reap_completed() {
        for (auto iterator = workers_.begin(); iterator != workers_.end();) {
            if (!iterator->done->load(std::memory_order_acquire)) {
                ++iterator;
                continue;
            }
            if (iterator->thread.joinable()) {
                iterator->thread.join();
            }
            const auto active = std::find(active_.begin(), active_.end(), iterator->transport);
            if (active != active_.end()) {
                active_.erase(active);
            }
            log_parts("[SERVER] Worker joined: ", iterator->peer);
            iterator = workers_.erase(iterator);
        }
    }

    bool try_start(AcceptedConnection connection) {
        auto transport = std::shared_ptr<ITransport>(std::move(connection.transport));
        if (active_.size() >= maximum_active_clients) {
            transport->cancel();
            log_line("[SERVER] Client rejected: active connection limit reached");
            return false;
        }

        auto done = std::make_shared<std::atomic<bool>>(false);
        std::string worker_peer = connection.peer;
        active_.push_back(transport);
        try {
            workers_.emplace_back();
            Worker& worker = workers_.back();
            worker.done = done;
            worker.transport = transport;
            worker.peer = std::move(connection.peer);
            std::string log_peer = std::move(worker_peer);
            worker.thread = std::thread(
                [transport, store = store_, done, peer = std::move(log_peer), listener = &listener_]() {
                try {
                    const ReceivedFile received = receive_file(
                        *transport, *store, [](std::uint64_t size) {
                            log_parts("[SERVER] Transfer started: ", size, " bytes");
                        });
                    log_parts("[SERVER] Transfer completed: ", received.path.string());
                } catch (const std::exception& error) {
                    log_parts("[SERVER] Transfer failed: ", error.what());
                } catch (...) {
                    log_line("[SERVER] Transfer failed: unknown error");
                }
                done->store(true, std::memory_order_release);
                listener->notify();
            });
        } catch (const std::exception& error) {
            active_.erase(std::remove(active_.begin(), active_.end(), transport), active_.end());
            if (!workers_.empty() && !workers_.back().thread.joinable() &&
                workers_.back().transport == transport) {
                workers_.pop_back();
            }
            log_parts("[SERVER] Could not start worker: ", error.what());
            return false;
        }
        return true;
    }

    void shutdown() noexcept {
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        listener_.cancel();
        for (const auto& transport : active_) {
            transport->cancel();
        }
        for (Worker& worker : workers_) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
        }
        workers_.clear();
        active_.clear();
    }

private:
    ITransportListener& listener_;
    std::shared_ptr<IFileStore> store_;
    std::vector<std::shared_ptr<ITransport>> active_;
    std::list<Worker> workers_;
    bool shutdown_ = false;
};

ServerWorkers::ServerWorkers(ITransportListener& listener, std::shared_ptr<IFileStore> store)
    : implementation_(std::make_unique<Implementation>(listener, std::move(store))) {}

ServerWorkers::~ServerWorkers() = default;

void ServerWorkers::reap_completed() {
    implementation_->reap_completed();
}

bool ServerWorkers::try_start(AcceptedConnection connection) {
    return implementation_->try_start(std::move(connection));
}

void ServerWorkers::shutdown() noexcept {
    implementation_->shutdown();
}

} // namespace file_transfer::application
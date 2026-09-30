#include "file_transfer/application/client_application.hpp"

#include "file_transfer/application/logging.hpp"
#include "file_transfer/application/unique_fd.hpp"
#include "file_transfer/transport/tcp_connector.hpp"
#include "file_transfer/transfer/transfer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <system_error>
#include <utility>

namespace file_transfer {

ClientApplication::ClientApplication(ClientOptions options) : options_(std::move(options)) {}

void ClientApplication::run() {
    const int descriptor = ::open(options_.file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open input file");
    }
    application::UniqueFd input(descriptor);
    const FileMetadata metadata = calculate_file_metadata(input.get());

    application::log_parts("[CLIENT] Connecting to ", options_.host, ":", options_.port);
    std::unique_ptr<ITransport> transport = connect_tcp(options_.host, options_.port);
    application::log_parts("[CLIENT] Transfer started: ", options_.file.string(), " (",
                           metadata.size, " bytes)");
    send_file(*transport, input.get(), metadata);
    application::log_line("[CLIENT] Transfer completed successfully");
}

} // namespace file_transfer
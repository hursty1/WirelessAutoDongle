#pragma once

#include <functional>
#include <string>

namespace iap2probe {
// Diagnostic link only: never sends a certificate, signature, or credentials.
enum class Result { AuthenticationRequired = 20, IdentificationRequired = 21,
                    Timeout = 22, Disconnected = 23, InvalidProtocol = 24,
                    PeerRejected = 25, IoError = 26 };
struct Limits {
    int detectionMs = 6000;
    int synchronizationMs = 6000;
    int controlMs = 10000;
    int retryMs = 1000;
};
using Report = std::function<void(const std::string&, const std::string&, const std::string&)>;
Result run(int socket, const Report& report, const Limits& limits = {});
}

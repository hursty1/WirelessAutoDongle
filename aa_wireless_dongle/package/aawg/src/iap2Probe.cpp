#include "iap2Probe.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <vector>

namespace iap2probe {
namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using Bytes = std::vector<uint8_t>;
constexpr size_t MaximumPacket = 4096;
constexpr uint8_t Syn = 0x80, Ack = 0x40, Reset = 0x10;
constexpr uint8_t ControlSession = 10;
constexpr std::array<uint8_t, 6> Detection = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};

struct Failure : std::runtime_error {
    Result result;
    Failure(Result r, const std::string& text) : std::runtime_error(text), result(r) {}
};

uint16_t word(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
bool checksum(const Bytes& bytes) {
    uint8_t sum = 0;
    for (auto b : bytes) sum = static_cast<uint8_t>(sum + b);
    return sum == 0;
}
void appendChecksum(Bytes& bytes) {
    uint8_t sum = 0;
    for (auto b : bytes) sum = static_cast<uint8_t>(sum + b);
    bytes.push_back(static_cast<uint8_t>(0 - sum));
}

bool ready(int fd, short events, Deadline deadline) {
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        pollfd descriptor{fd, events, 0};
        int result = poll(&descriptor, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) throw Failure(Result::IoError, std::string("poll: ") + strerror(errno));
        if (result == 0) return false;
        if (descriptor.revents & events) return true;
        if (descriptor.revents & POLLNVAL) throw Failure(Result::IoError, "invalid transport descriptor");
        if (descriptor.revents & (POLLHUP | POLLERR)) throw Failure(Result::Disconnected, "transport closed");
    }
}

uint8_t receiveByte(int fd, Deadline deadline) {
    while (ready(fd, POLLIN, deadline)) {
        uint8_t value;
        auto n = recv(fd, &value, 1, MSG_DONTWAIT);
        if (n == 1) return value;
        if (n == 0) throw Failure(Result::Disconnected, "peer disconnected");
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        throw Failure(Result::IoError, std::string("receive: ") + strerror(errno));
    }
    throw Failure(Result::Timeout, "no response before stage deadline");
}

void sendBytes(int fd, const Bytes& bytes, Deadline deadline) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (!ready(fd, POLLOUT, deadline)) throw Failure(Result::Timeout, "transport write deadline");
        auto n = send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) offset += static_cast<size_t>(n);
        else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else throw Failure(Result::IoError, "transport write failed");
    }
}

struct Packet { uint8_t flags, sequence, acknowledgement, session; Bytes payload; };

Bytes encode(uint8_t flags, uint8_t sequence, uint8_t acknowledgement, const Bytes& payload = {}) {
    size_t length = 9 + (payload.empty() ? 0 : payload.size() + 1);
    Bytes header{0xff, 0x5a, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length),
                 flags, sequence, acknowledgement, 0};
    appendChecksum(header);
    if (!payload.empty()) {
        Bytes body = payload;
        appendChecksum(body);
        header.insert(header.end(), body.begin(), body.end());
    }
    return header;
}

Packet receivePacket(int fd, Deadline deadline) {
    uint8_t previous = 0;
    while (true) {
        uint8_t value = receiveByte(fd, deadline);
        if (previous == 0xff && value == 0x5a) break;
        previous = value;
    }
    Bytes header{0xff, 0x5a};
    while (header.size() < 9) header.push_back(receiveByte(fd, deadline));
    if (!checksum(header)) throw Failure(Result::InvalidProtocol, "invalid iAP2 header checksum");
    size_t length = word(header.data() + 2);
    if (length < 9 || length > MaximumPacket) throw Failure(Result::InvalidProtocol, "iAP2 length outside advertised limit");
    Bytes body;
    while (body.size() < length - 9) body.push_back(receiveByte(fd, deadline));
    if (!body.empty()) {
        if (!checksum(body)) throw Failure(Result::InvalidProtocol, "invalid iAP2 payload checksum");
        body.pop_back();
    }
    return {header[4], header[5], header[6], header[7], body};
}

uint8_t controlSession(const Bytes& payload) {
    if (payload.size() < 13 || payload[0] != 1 || (payload.size() - 10) % 3 != 0 ||
        payload[1] == 0 || word(payload.data() + 2) < 64)
        throw Failure(Result::InvalidProtocol, "invalid link synchronization parameters");
    for (size_t i = 10; i < payload.size(); i += 3) {
        if (payload[i] != 0 && payload[i + 1] == 0 && payload[i + 2] == 1) return payload[i];
    }
    throw Failure(Result::InvalidProtocol, "peer offered no supported control session");
}
}

Result run(int fd, const Report& report, const Limits& limits) {
    std::string stage = "iap2_detection";
    try {
        if (limits.detectionMs <= 0 || limits.synchronizationMs <= 0 || limits.controlMs <= 0 || limits.retryMs <= 0)
            throw Failure(Result::InvalidProtocol, "probe deadlines must be positive");
        report(stage, "START", "RFCOMM connected; probing iAP2 support");
        auto deadline = Clock::now() + std::chrono::milliseconds(limits.detectionMs);
        auto nextSend = Clock::now();
        size_t matched = 0;
        while (matched < Detection.size()) {
            if (Clock::now() >= deadline) throw Failure(Result::Timeout, "iAP2 detection marker not received");
            if (Clock::now() >= nextSend) {
                sendBytes(fd, Bytes(Detection.begin(), Detection.end()), deadline);
                nextSend = Clock::now() + std::chrono::milliseconds(limits.retryMs);
            }
            try {
                uint8_t value = receiveByte(fd, std::min(deadline, nextSend));
                matched = value == Detection[matched] ? matched + 1 : (value == Detection[0] ? 1 : 0);
            } catch (const Failure& f) {
                if (f.result != Result::Timeout) throw;
            }
        }
        report(stage, "OK", "peer returned the iAP2 support marker");

        stage = "iap2_link";
        report(stage, "START", "negotiating control session; no authentication provider configured");
        // One control session, bounded packet/window and retry parameters.
        const Bytes parameters{1, 4, 0x10, 0, 0x03, 0xe8, 0, 100, 3, 1, ControlSession, 0, 1};
        deadline = Clock::now() + std::chrono::milliseconds(limits.synchronizationMs);
        nextSend = Clock::now();
        bool sawSyn = false, acknowledged = false;
        uint8_t lastPeerSequence = 0, peerControl = ControlSession;
        std::optional<Packet> pending;
        while (!sawSyn || !acknowledged) {
            if (Clock::now() >= deadline) throw Failure(Result::Timeout, "link synchronization not completed");
            if (Clock::now() >= nextSend) {
                sendBytes(fd, encode(Syn, 0, lastPeerSequence, parameters), deadline);
                nextSend = Clock::now() + std::chrono::milliseconds(limits.retryMs);
            }
            // Wait for whole frames against the stage deadline: a fragmented
            // frame must not be discarded when the retransmit timer expires.
            if (!ready(fd, POLLIN, std::min(deadline, nextSend))) continue;
            Packet packet = receivePacket(fd, deadline);
            if (packet.flags & Reset) throw Failure(Result::PeerRejected, "peer reset during link setup");
            if (packet.flags & Syn) {
                peerControl = controlSession(packet.payload);
                sawSyn = true;
                lastPeerSequence = packet.sequence;
                sendBytes(fd, encode(Ack, 0, lastPeerSequence), deadline);
            }
            if ((packet.flags & Ack) && packet.acknowledgement == 0) acknowledged = true;
            if (!(packet.flags & Syn) && !packet.payload.empty()) {
                if (!sawSyn) throw Failure(Result::InvalidProtocol, "data before peer synchronization");
                pending = packet;
            }
        }
        report(stage, "OK", "peer synchronization and acknowledgement observed");

        stage = "iap2_control";
        report(stage, "START", "waiting for phone control request; authentication payloads are not logged");
        deadline = Clock::now() + std::chrono::milliseconds(limits.controlMs);
        Bytes messages;
        while (true) {
            Packet packet = pending ? std::move(*pending) : receivePacket(fd, deadline);
            pending.reset();
            if (packet.flags & Reset) throw Failure(Result::PeerRejected, "peer reset control session");
            if (packet.flags & Syn) {
                sendBytes(fd, encode(Ack, 0, lastPeerSequence), deadline);
                continue;
            }
            if (packet.payload.empty()) continue;
            if (packet.sequence != static_cast<uint8_t>(lastPeerSequence + 1)) {
                sendBytes(fd, encode(Ack, 0, lastPeerSequence), deadline);
                continue;
            }
            lastPeerSequence = packet.sequence;
            sendBytes(fd, encode(Ack, 0, lastPeerSequence), deadline);
            if (packet.session != peerControl && packet.session != ControlSession) continue;
            if (messages.size() + packet.payload.size() > MaximumPacket * 2)
                throw Failure(Result::InvalidProtocol, "control buffer limit exceeded");
            messages.insert(messages.end(), packet.payload.begin(), packet.payload.end());
            while (messages.size() >= 6) {
                if (word(messages.data()) != 0x4040) throw Failure(Result::InvalidProtocol, "invalid control message marker");
                size_t size = word(messages.data() + 2);
                if (size < 6 || size > MaximumPacket) throw Failure(Result::InvalidProtocol, "invalid control message length");
                if (messages.size() < size) break;
                uint16_t id = word(messages.data() + 4);
                if (id == 0xaa00) {
                    report("authentication_request", "OK", "phone requested accessory authentication certificate");
                    report("authentication_provider", "BLOCKED", "no MFi authentication provider configured; probe sends no certificate/signature");
                    return Result::AuthenticationRequired;
                }
                if (id == 0x1d00) {
                    report("identification_request", "OK", "phone requested accessory identification");
                    report("identification", "BLOCKED", "identification/session backend not implemented in this probe");
                    return Result::IdentificationRequired;
                }
                if (id == 0xaa04) throw Failure(Result::PeerRejected, "phone reported authentication failure");
                report(stage, "OBSERVED", "control message id=" + std::to_string(id) + "; payload omitted");
                messages.erase(messages.begin(), messages.begin() + size);
            }
        }
    } catch (const Failure& f) {
        const char* outcome = f.result == Result::Timeout ? "TIMEOUT" :
                              f.result == Result::Disconnected ? "DISCONNECTED" : "FAIL";
        report(stage, outcome, f.what());
        return f.result;
    }
}
}

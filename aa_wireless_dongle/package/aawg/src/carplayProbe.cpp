#include "bluetoothCommon.h"
#include "common.h"
#include "iap2Probe.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <stdexcept>
#include <unistd.h>
#include <vector>

extern char** environ;
namespace {
volatile sig_atomic_t stopping = 0;
void stopSignal(int) { stopping = 1; }
constexpr const char* ServerUuid = "00000000-deca-fade-deca-deafdecacaff";
constexpr const char* ClientUuid = "00000000-deca-fade-deca-deafdecacafe";
constexpr const char* ProfileInterface = "org.bluez.Profile1";

void report(unsigned attempt, const std::string& stage, const std::string& outcome, const std::string& detail) {
    Logger::instance()->info("CarPlay probe attempt=%u stage=%s outcome=%s detail=%s\n",
                             attempt, stage.c_str(), outcome.c_str(), detail.c_str());
    std::vector<std::string> environment;
    for (char** p = environ; *p; ++p) {
        if (std::string(*p).rfind("AAWG_LOG_ATTEMPT=", 0) != 0) environment.emplace_back(*p);
    }
    environment.push_back("AAWG_LOG_ATTEMPT=" + std::to_string(attempt));
    std::vector<char*> env;
    for (auto& item : environment) env.push_back(item.data());
    env.push_back(nullptr);
    const char* arguments[] = {"aawg-logs", "event", "phone", stage.c_str(), outcome.c_str(), detail.c_str(), nullptr};
    pid_t child;
    int result = posix_spawn(&child, "/usr/sbin/aawg-logs", nullptr, nullptr,
                             const_cast<char**>(arguments), env.data());
    if (result == 0) {
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    } else {
        Logger::instance()->info("CarPlay probe: persistent stage helper unavailable: %d\n", result);
    }
}

class Sessions {
    std::mutex lifecycle;
    std::mutex mutex;
    std::thread worker;
    int active = -1;
    unsigned attempt = 0;
public:
    ~Sessions() { stop(); }
    void accept(int original) {
        std::lock_guard<std::mutex> operation(lifecycle);
        int fd = fcntl(original, F_DUPFD_CLOEXEC, 3);
        if (fd < 0) { report(0, "rfcomm", "FAIL", "could not duplicate BlueZ transport descriptor"); return; }
        std::lock_guard<std::mutex> lock(mutex);
        if (active >= 0 || stopping) { close(fd); return; }
        if (worker.joinable()) worker.join();
        active = fd;
        unsigned number = ++attempt;
        worker = std::thread([this, fd, number] {
            try {
                report(number, "rfcomm", "OK", "BlueZ delivered an authenticated Bluetooth transport");
                auto events = [number](const std::string& s, const std::string& o, const std::string& d) {
                    report(number, s, o, d);
                };
                auto result = iap2probe::run(fd, events);
                // Do not overwrite the actionable final stage with a generic
                // disconnect event; the summary must retain the blocked stage.
                Logger::instance()->info("CarPlay probe attempt=%u finished result=%d\n", number, static_cast<int>(result));
            } catch (const std::exception& e) {
                report(number, "probe", "FAIL", e.what());
            }
            std::lock_guard<std::mutex> finished(mutex);
            shutdown(fd, SHUT_RDWR);
            close(fd);
            active = -1;
        });
    }
    void stop() {
        std::lock_guard<std::mutex> operation(lifecycle);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (active >= 0) shutdown(active, SHUT_RDWR);
        }
        if (worker.joinable()) worker.join();
    }
};

class Profile : public DBus::Object {
    std::shared_ptr<Sessions> sessions;
    Profile(const DBus::Path& path, std::shared_ptr<Sessions> state) : DBus::Object(path), sessions(std::move(state)) {
        create_method<void(void)>(ProfileInterface, "Release", sigc::mem_fun(*this, &Profile::release));
        create_method<void(DBus::Path, std::shared_ptr<DBus::FileDescriptor>, DBus::Properties)>(
            ProfileInterface, "NewConnection", sigc::mem_fun(*this, &Profile::connection));
        create_method<void(DBus::Path)>(ProfileInterface, "RequestDisconnection", sigc::mem_fun(*this, &Profile::disconnect));
    }
    void release() { sessions->stop(); }
    void connection(DBus::Path, std::shared_ptr<DBus::FileDescriptor> fd, DBus::Properties) {
        sessions->accept(fd->descriptor());
    }
    void disconnect(DBus::Path) { sessions->stop(); }
public:
    static std::shared_ptr<Profile> create(const DBus::Path& path, std::shared_ptr<Sessions> sessions) {
        return std::shared_ptr<Profile>(new Profile(path, sessions));
    }
};
}

int main() {
    signal(SIGTERM, stopSignal);
    signal(SIGINT, stopSignal);
    signal(SIGPIPE, SIG_IGN);
    try {
        auto dispatcher = DBus::StandaloneDispatcher::create();
        auto bus = dispatcher->create_connection(DBus::BusType::SYSTEM);
        auto root = bus->create_object_proxy("org.bluez", "/");
        auto getObjects = root->create_method<DBus::ManagedObjects(void)>("org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        std::string adapterPath;
        for (auto const& [path, interfaces] : (*getObjects)()) {
            if (interfaces.count("org.bluez.Adapter1")) { adapterPath = path; break; }
        }
        if (adapterPath.empty()) { report(0, "bluetooth_adapter", "FAIL", "BlueZ has no Bluetooth adapter"); return 1; }
        auto adapter = bus->create_object_proxy("org.bluez", adapterPath);
        auto powered = adapter->create_property<bool>("org.bluez.Adapter1", "Powered");
        auto pairable = adapter->create_property<bool>("org.bluez.Adapter1", "Pairable");
        auto discoverable = adapter->create_property<bool>("org.bluez.Adapter1", "Discoverable");
        auto discoverableTimeout = adapter->create_property<uint32_t>("org.bluez.Adapter1", "DiscoverableTimeout");
        auto alias = adapter->create_property<std::string>("org.bluez.Adapter1", "Alias");
        powered->set_value(true);
        alias->set_value("CarPlay-Probe-" + Config::instance()->getUniqueSuffix());
        discoverableTimeout->set_value(0);
        pairable->set_value(true);
        discoverable->set_value(true);

        auto sessions = std::make_shared<Sessions>();
        const DBus::Path serverPath("/com/aawgd/carplay_probe/server");
        const DBus::Path clientPath("/com/aawgd/carplay_probe/client");
        auto server = Profile::create(serverPath, sessions);
        auto client = Profile::create(clientPath, sessions);
        for (auto profile : {server, client}) {
            if (bus->register_object(profile, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success)
                throw std::runtime_error("could not export BlueZ profile object");
        }
        auto manager = bus->create_object_proxy("org.bluez", "/org/bluez");
        auto registerProfile = manager->create_method<void(DBus::Path, std::string, DBus::Properties)>("org.bluez.ProfileManager1", "RegisterProfile");
        auto unregisterProfile = manager->create_method<void(DBus::Path)>("org.bluez.ProfileManager1", "UnregisterProfile");
        (*registerProfile)(serverPath, ServerUuid, {
            {"Name", DBus::Variant("iAP2 diagnostic probe")},
            {"Role", DBus::Variant("server")},
            {"Channel", DBus::Variant(uint16_t(3))},
            {"RequireAuthentication", DBus::Variant(true)},
        });
        (*registerProfile)(clientPath, ClientUuid, {
            {"Name", DBus::Variant("iAP2 diagnostic reconnect")},
            {"Role", DBus::Variant("client")},
            {"AutoConnect", DBus::Variant(true)},
            {"RequireAuthentication", DBus::Variant(true)},
        });
        report(0, "probe_ready", "OK", "iAP2 RFCOMM profiles registered; pair iPhone in Bluetooth settings");
        report(0, "rfcomm_wait", "START", "no MFi provider, Wi-Fi CarPlay handover, or truck USB session backend; handshake probe only");
        unsigned seconds = 0;
        while (!stopping) {
            sleep(1);
            if (++seconds == 120) Logger::instance()->info("CarPlay probe: listening; inspect attempt events to distinguish RFCOMM activity from pairing alone\n");
        }
        (*unregisterProfile)(serverPath);
        (*unregisterProfile)(clientPath);
        sessions->stop();
        discoverable->set_value(false);
        return 0;
    } catch (const std::exception& e) {
        report(0, "probe_startup", "FAIL", e.what());
        return 1;
    }
}

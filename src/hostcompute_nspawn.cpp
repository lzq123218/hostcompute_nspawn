
#include "hostcompute_nspawn.h"

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#include "staticlib/config.hpp"
#include "staticlib/io.hpp"
#include "staticlib/utils.hpp"
#include "staticlib/serialization.hpp"

#include "NSpawnConfig.hpp"
#include "NSpawnException.hpp"
#include "ContainerConfig.hpp"
#include "ContainerId.hpp"
#include "ContainerLayer.hpp"
#include "ProcessConfig.hpp"


namespace { // anonymous 

namespace sc = staticlib::config;
namespace si = staticlib::io;
namespace su = staticlib::utils;
namespace ss = staticlib::serialization;

} // namespace

namespace nspawn {

enum class HcsErrors : uint32_t {
    OPERATION_PENDING = 0xC0370103
};

enum class NotificationType : uint32_t {
    // Notifications for HCS_SYSTEM handles
    SYSTEM_EXIT = 0x00000001,
    SYSTEM_CREATE_COMPLETE = 0x00000002,
    SYSTEM_START_COMPLETE = 0x00000003,
    SYSTEM_PAUSE_COMPLETE = 0x00000004,
    SYSTEM_RESUME_COMPLETE = 0x00000005,

    // Notifications for HCS_PROCESS handles
    PROCESS_EXIT = 0x00010000,

    // Common notifications
    COMMON_INVALID = 0x00000000,
    COMMON_SERVICE_DISCONNECT = 0x01000000
};

class CallbackLatch {
    std::mutex mutex;
    std::condition_variable system_create_cv;
    std::atomic<bool> system_create_flag = false;
    std::condition_variable system_start_cv;
    std::atomic<bool> system_start_flag = false;
    std::condition_variable system_exit_cv;
    std::atomic<bool> system_exit_flag = false;
    std::condition_variable process_exit_cv;
    std::atomic<bool> process_exit_flag = false;

public:
    CallbackLatch() { }

    CallbackLatch(const CallbackLatch&) = delete;

    CallbackLatch& operator=(const CallbackLatch&) = delete;

    void lock() {
        std::unique_lock<std::mutex> guard{ mutex };
        guard.release();
    }

    void await(NotificationType nt) {
        switch (nt) {
        case NotificationType::SYSTEM_CREATE_COMPLETE: await_internal(system_create_cv, system_create_flag); break;
        case NotificationType::SYSTEM_START_COMPLETE: await_internal(system_start_cv, system_start_flag); break;
        case NotificationType::SYSTEM_EXIT: await_internal(system_exit_cv, system_exit_flag); break;
        case NotificationType::PROCESS_EXIT: await_internal(process_exit_cv, process_exit_flag); break;
        default: throw NSpawnException(TRACEMSG("Unsupported notification type"));
        }
    }

    void unlock(NotificationType nt) {
        switch (nt) {
        case NotificationType::SYSTEM_CREATE_COMPLETE: unlock_internal(system_create_cv, system_create_flag); break;
        case NotificationType::SYSTEM_START_COMPLETE: unlock_internal(system_start_cv, system_start_flag); break;
        case NotificationType::SYSTEM_EXIT: unlock_internal(system_exit_cv, system_exit_flag); break;
        case NotificationType::PROCESS_EXIT: unlock_internal(process_exit_cv, process_exit_flag); break;
        default: { /* ignore */ }
        }
    }

private:
    void await_internal(std::condition_variable& cv, std::atomic<bool>& flag) {
        std::unique_lock<std::mutex> guard{mutex, std::adopt_lock};
        cv.wait(guard, [&flag]{
            return flag.load();
        });
    }

    void unlock_internal(std::condition_variable& cv, std::atomic<bool>& flag) {
        static bool the_false = false;
        if (flag.compare_exchange_strong(the_false, true)) {
            std::unique_lock<std::mutex> guard{mutex};
            cv.notify_all();
        }
    }
};

std::vector<ContainerLayer> collect_acsendant_layers(const std::string& base_path,
        const std::string& parent_layer_name) {
    std::vector<ContainerLayer> res;
    res.emplace_back(base_path, parent_layer_name);
    // todo: find out on disk
    res.emplace_back(base_path, "c98833436817d72e5a11b062890502b31fd5cfcb7b5b5047bcf8cc430d7a2166");
    return res;
}

void spawn_and_wait(const NSpawnConfig& config) {

    std::cout << ss::dump_json_to_string(config.to_json()) << std::endl;

    // prepare DriverInfo
    std::string base_path = su::strip_filename(config.parent_layer_directory);
    std::string parent_layer_name = su::strip_parent_dir(config.parent_layer_directory);
    std::wstring wbp = su::widen(base_path);
    DriverInfo driver_info;
    std::memset(std::addressof(driver_info), '\0', sizeof(DriverInfo));
    driver_info.Flavour = GraphDriverType::FilterDriver;
    driver_info.HomeDir = wbp.c_str();

    // prepare acsendant layers
    std::vector<ContainerLayer> acsendant_layers = collect_acsendant_layers(base_path, parent_layer_name);
    std::vector<WC_LAYER_DESCRIPTOR> acsendant_descriptors;
    for (auto& la : acsendant_layers) {
        acsendant_descriptors.emplace_back(la.to_descriptor());
    }

    auto rng = su::RandomStringGenerator("0123456789abcdef");
    auto layer = ContainerLayer(base_path, std::string("nspawn_") + utils::current_datetime() + "_" + rng.generate(26));

    { // create layer
        std::wstring wname = su::widen(layer.get_name());
        std::wstring wparent = su::widen(parent_layer_name);
        auto res = ::CreateSandboxLayer(std::addressof(driver_info), wname.c_str(), wparent.c_str(),
                acsendant_descriptors.data(), static_cast<uint32_t>(acsendant_descriptors.size()));
        if (0 != res) {
            throw NSpawnException(TRACEMSG("'CreateSandboxLayer' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " parent_layer_name: [" + parent_layer_name + "]," +
                " error: [" + su::errcode_to_string(res) + "]"));
        }
        std::cout << "Layer created, name: [" << layer.get_name() << "]" << std::endl;
    }

    { // activate layer
        std::wstring wname = su::widen(layer.get_name());
        auto res = ::ActivateLayer(std::addressof(driver_info), wname.c_str());
        if (0 != res) {
            throw NSpawnException(TRACEMSG("'ActivateLayer' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " error: [" + su::errcode_to_string(res) + "]"));
        }
        std::cout << "Layer activated, name: [" << layer.get_name() << "]" << std::endl;
    }

    { // prepare layer
        std::wstring wname = su::widen(layer.get_name());
        auto res = ::PrepareLayer(std::addressof(driver_info), wname.c_str(), 
                acsendant_descriptors.data(), static_cast<uint32_t>(acsendant_descriptors.size()));
        if (0 != res) {
            throw NSpawnException(TRACEMSG("'PrepareLayer' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " error: [" + su::errcode_to_string(res) + "]"));
        }
        std::cout << "Layer prepared, name: [" << layer.get_name() << "]" << std::endl;
    }

    std::string volume_path;

    { // find out mount path
        std::wstring wname = su::widen(layer.get_name());
        std::wstring path;
        path.resize(MAX_PATH);
        uint32_t length = MAX_PATH;
        auto res = ::GetLayerMountPath(std::addressof(driver_info), wname.c_str(), 
                std::addressof(length), std::addressof(path.front()));
        if (0 != res) {
            throw NSpawnException(TRACEMSG("'GetLayerMountPath' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " error: [" + su::errcode_to_string(res) + "]"));
        }
        volume_path = su::narrow(path.c_str());
        std::cout << "Found volume path: [" << volume_path << "]" <<
            " for layer, name: [" << layer.get_name() << "]" << std::endl;
    }

    std::vector<ContainerLayer> alpass;
    for (auto& la : acsendant_layers) {
        alpass.emplace_back(la.clone());
    }
    HANDLE computeSystem = nullptr;

    { // create container
        auto container_config = ContainerConfig(base_path, config.process_directory, config.mapped_directory,
                volume_path, layer.clone(), std::move(alpass), rng.generate(8));
        std::wstring wname = su::widen(layer.get_name());
        std::string conf = ss::dump_json_to_string(container_config.to_json());
        std::wstring wconf = su::widen(conf);
        HANDLE identity = nullptr;
        wchar_t* result = nullptr;
        auto res = ::HcsCreateComputeSystem(wname.c_str(), wconf.c_str(), identity, 
                std::addressof(computeSystem), std::addressof(result));
        if (static_cast<uint32_t>(HcsErrors::OPERATION_PENDING) != res) {
            throw NSpawnException(TRACEMSG("'HcsCreateComputeSystem' failed," +
                " config: [" + conf + "]," +
                " error: [" + su::errcode_to_string(res) + "]"));
        }
        std::cout << "Container created, name: [" << layer.get_name() << "]" << std::endl;
    }

    HANDLE cs_callback_handle = nullptr;
    CallbackLatch cs_latch;
    auto cs_callback = [](uint32_t notificationType, void* context, int32_t notificationStatus, wchar_t* notificationData) {
        std::string data = nullptr != notificationData ? su::narrow(notificationData) : "";
        std::cout << "CS notification received, notificationType: [" << sc::to_string(notificationType) << "]," <<
            " notificationStatus: [" << notificationStatus << "]," <<
            " notificationData: [" << data << "]" << std::endl;
        CallbackLatch& la = *static_cast<CallbackLatch*> (context);
        la.unlock(static_cast<NotificationType>(notificationType));
    };

    { // register callback
        cs_latch.lock();
        auto res = ::HcsRegisterComputeSystemCallback(computeSystem, cs_callback, static_cast<void*>(std::addressof(cs_latch)),
                std::addressof(cs_callback_handle));
        if (0 == res) {
            std::cout << "CS callback registered successfully, name: [" << layer.get_name() << "]" << std::endl;
            cs_latch.await(NotificationType::SYSTEM_CREATE_COMPLETE);
            std::cout << "CS create latch unlocked" << std::endl;
        }
        else {
            std::cerr << "ERROR: 'HcsRegisterComputeSystemCallback' failed, name: [" << layer.get_name() << "]" <<
                " error: [" << su::errcode_to_string(res) << "]" << std::endl;
        }
    }

    { // enumerate
        std::wstring query = su::widen("{}");
        wchar_t* computeSystems = nullptr;
        wchar_t* result = nullptr;
        auto res = ::HcsEnumerateComputeSystems(query.c_str(),
                std::addressof(computeSystems), std::addressof(result));
        if (0 != res) {
            throw NSpawnException(TRACEMSG("'HcsEnumerateComputeSystems' failed," +
                    " error: [" + su::errcode_to_string(res) + "]"));
        }
        std::cout << su::narrow(computeSystems) << std::endl;
    }

    { // start
        std::wstring options = su::widen("");
        wchar_t* result = nullptr;
        cs_latch.lock();
        auto res = ::HcsStartComputeSystem(computeSystem, options.c_str(), std::addressof(result));
        if (static_cast<uint32_t>(HcsErrors::OPERATION_PENDING) != res) {
            throw NSpawnException(TRACEMSG("'HcsStartComputeSystem' failed," +
                " error: [" + su::errcode_to_string(res) + "]"));
        }
        cs_latch.await(NotificationType::SYSTEM_START_COMPLETE);
        std::cout << "Container started, name: [" << layer.get_name() << "]" << std::endl;
    }

    HANDLE process = nullptr;

    { // start process
        auto pcfg = ProcessConfig(config.process_executable, config.mapped_directory, config.stdout_filename);
        std::string pcfg_json = ss::dump_json_to_string(pcfg.to_json());
        std::cout << pcfg_json << std::endl;
        std::wstring wpcfg_json = su::widen(pcfg_json);
        HCS_PROCESS_INFORMATION hpi;
        std::memset(std::addressof(hpi), '\0', sizeof(HCS_PROCESS_INFORMATION));
        wchar_t* result = nullptr;
        auto res = ::HcsCreateProcess(computeSystem, wpcfg_json.c_str(), std::addressof(hpi),
                std::addressof(process), std::addressof(result));
        if (0 != res) {
            throw NSpawnException(TRACEMSG("'HcsCreateProcess' failed," +
                " config: [" + pcfg_json + "]," +
                " error: [" + su::errcode_to_string(res) + "]"));
        }
        std::cout << "Process created" << std::endl;
    }

    HANDLE process_callback_handle;
    CallbackLatch process_latch;

    { // process callback
        cs_latch.lock();
        auto res = ::HcsRegisterProcessCallback(process, cs_callback, std::addressof(cs_latch), std::addressof(process_callback_handle));
        if (0 == res) {
            std::cout << "Process callback registered successfully, name: [" << layer.get_name() << "]" << std::endl;
            cs_latch.await(NotificationType::PROCESS_EXIT);
            std::cout << "Process create latch unlocked" << std::endl;
        }
        else {
            std::cerr << "ERROR: 'HcsRegisterProcessCallback' failed, name: [" << layer.get_name() << "]" <<
                " error: [" << su::errcode_to_string(res) << "]" << std::endl;
        }
    }

    { // terminate
        std::wstring options = su::widen("{}");
        wchar_t* result = nullptr;
        cs_latch.lock();
        auto res = ::HcsTerminateComputeSystem(computeSystem, options.c_str(), std::addressof(result));
        if (static_cast<uint32_t>(HcsErrors::OPERATION_PENDING) == res) {
            cs_latch.await(NotificationType::SYSTEM_EXIT);
            std::cout << "Container terminated, name: [" << layer.get_name() << "]" << std::endl;
        } else {
            std::cerr << "ERROR: 'HcsTerminateComputeSystem' failed, name: [" << layer.get_name() << "]" << 
                    " error: [" << su::errcode_to_string(res) << "]" << std::endl;
        }
    }

    { // unprepare layer
        std::wstring wname = su::widen(layer.get_name());
        auto res = ::UnprepareLayer(std::addressof(driver_info), wname.c_str());
        if (0 == res) {
            std::cout << "Layer unprepared, name: [" << layer.get_name() << "]" << std::endl;
        }
        else {
            std::cerr << "ERROR: 'UnprepareLayer' failed, name: [" << layer.get_name() << "]" <<
                " error: [" << su::errcode_to_string(res) << "]" << std::endl;
        }
    }

    { // deactivate layer
        std::wstring wname = su::widen(layer.get_name());
        auto res = ::DeactivateLayer(std::addressof(driver_info), wname.c_str());
        if (0 == res) {
            std::cout << "Layer deactivated, name: [" << layer.get_name() << "]" << std::endl;
        }
        else {
            std::cerr << "ERROR: 'DeactivateLayer' failed, name: [" << layer.get_name() << "]" <<
                " error: [" << su::errcode_to_string(res) << "]" << std::endl;
        }
    }

    { // destroy layer
        std::wstring wname = su::widen(layer.get_name());
        auto res = ::DestroyLayer(std::addressof(driver_info), wname.c_str());
        if (0 == res) {
            std::cout << "Layer destroyed, name: [" << layer.get_name() << "]" << std::endl;
        }
        else {
            std::cerr << "ERROR: 'DestroyLayer' failed, name: [" << layer.get_name() << "]" <<
                " error: [" << su::errcode_to_string(res) << "]" << std::endl;
        }
    }

    std::cout << "SHUTDOWN" << std::endl;
}

} // namespace
 
char* hostcompute_nspawn(const char* config_json, int config_json_len) /* noexcept */ {
    if (nullptr == config_json) return su::alloc_copy(TRACEMSG("Null 'config_json' parameter specified"));
    if (!su::is_positive_uint32(config_json_len)) return su::alloc_copy(TRACEMSG(
            "Invalid 'config_json_len' parameter specified: [" + sc::to_string(config_json_len) + "]"));
    try {
        auto src = si::array_source(config_json, config_json_len);
        auto loaded = ss::load_json(src);
        auto config = nspawn::NSpawnConfig(loaded);
        nspawn::spawn_and_wait(config);
        return nullptr;
    }
    catch (const std::exception& e) {
        return su::alloc_copy(TRACEMSG(e.what() + "\nException raised"));
    }
}

void hostcompute_nspawn_free(char* err_message) /* noexcept */ {
    std::free(err_message);
}

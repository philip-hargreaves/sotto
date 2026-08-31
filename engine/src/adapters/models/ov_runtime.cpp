#include "adapters/models/ov_runtime.hpp"

#include <stdexcept>

namespace ambient::models {

std::string OvRuntime::ResolveDevice(const std::string& requested) {
    if (requested == "CPU") {
        return "CPU";
    }
    if (requested == "GPU" || requested == "NPU") {
        std::string listing;
        for (const auto& device : core_.get_available_devices()) {
            if (device.rfind(requested, 0) != 0) continue;
            const auto name = core_.get_property(device, ov::device::full_name);
            listing += " " + device + "=" + name;
            if (requested == "NPU" || name.find("Intel") != std::string::npos) {
                return device;
            }
        }
        throw std::runtime_error("no Intel " + requested +
                                 " available; found:" + (listing.empty() ? " nothing" : listing));
    }
    throw std::runtime_error("unsupported device in manifest: " + requested);
}

std::map<std::string, std::string> OvRuntime::DescribeDevices() {
    std::map<std::string, std::string> devices;
    for (const auto& device : core_.get_available_devices()) {
        try {
            std::string name = core_.get_property(device, ov::device::full_name);
            if (device.rfind("NPU", 0) == 0) {
                try {
                    name += " (arch " + core_.get_property(device, ov::device::architecture) + ")";
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                }
            }
            devices[device] = name;
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }
    return devices;
}

LoadedModel OvRuntime::Load(const ModelStore& store, std::string_view task, std::string_view tier,
                            const std::string& xml_name) {
    const ModelInfo& info = store.Resolve(task, tier);
    store.Verify(info);

    const auto xml = info.dir / xml_name;
    if (!std::filesystem::exists(xml)) {
        throw std::runtime_error(info.id + ": no such model file " + xml_name);
    }

    LoadedModel loaded;
    loaded.device = ResolveDevice(info.device);
    const auto start = std::chrono::steady_clock::now();
    // Same convention as the whisper pipeline: first launch compiles and
    // exports, every later launch imports the cached blob
    loaded.model = core_.compile_model(xml.string(), loaded.device,
                                       ov::AnyMap{{"CACHE_DIR", (info.dir / ".cache").string()}});
    loaded.load_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return loaded;
}

}  // namespace ambient::models

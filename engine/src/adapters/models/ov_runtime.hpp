#pragma once

#include <chrono>
#include <map>
#include <openvino/openvino.hpp>
#include <string>
#include <string_view>

#include "adapters/models/model_store.hpp"

namespace sotto::models {

struct LoadedModel {
    ov::CompiledModel model;
    std::string device;  // the concrete device compiled for, e.g. GPU.1
    std::chrono::milliseconds load_time{0};
};

// Compiles store-cleared models for their manifest device. Selection is
// explicit: "GPU" means the Intel GPU found by vendor name (multi-GPU
// machines enumerate GPU.0/GPU.1), and an unavailable device is a loud
// error naming what exists. No fallback of any kind.
class OvRuntime {
   public:
    LoadedModel Load(const ModelStore& store, std::string_view task, std::string_view tier,
                     const std::string& xml_name);

    std::string ResolveDevice(const std::string& requested);

    // Device id -> driver-reported full name (NPU with its architecture)
    std::map<std::string, std::string> DescribeDevices();

   private:
    ov::Core core_;
};

}  // namespace sotto::models

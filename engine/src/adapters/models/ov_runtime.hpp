#pragma once

#include <chrono>
#include <map>
#include <openvino/openvino.hpp>
#include <string>
#include <string_view>

#include "adapters/models/model_store.hpp"

namespace ambient::models {

struct LoadedModel {
    ov::CompiledModel model;
    std::string device;  // the concrete device compiled for, e.g. GPU.1
    std::chrono::milliseconds load_time{0};
};

// Compiles cleared models for their manifest device; an unavailable device
// is a loud error naming what exists, no fallback
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

}  // namespace ambient::models

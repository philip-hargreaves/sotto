#pragma once

#include <string>
#include <vector>

namespace ambient {

// Removes "--name value" from args and returns the value; "" if absent
inline std::string TakeFlag(std::vector<std::string>& args, const std::string& name) {
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (*it == name && std::next(it) != args.end()) {
            std::string value = *std::next(it);
            args.erase(it, it + 2);
            return value;
        }
    }
    return {};
}

}  // namespace ambient

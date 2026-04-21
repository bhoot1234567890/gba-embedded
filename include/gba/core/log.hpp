#pragma once

#include <string_view>

#include "gba/core/types.hpp"

namespace gba {

class TraceLogger {
public:
    virtual ~TraceLogger() = default;
    virtual void log(std::string_view channel, std::string_view message) = 0;
};

class NullLogger final : public TraceLogger {
public:
    void log(std::string_view channel, std::string_view message) override;
};

class StdoutLogger final : public TraceLogger {
public:
    void log(std::string_view channel, std::string_view message) override;
};

}  // namespace gba

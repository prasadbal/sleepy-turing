#pragma once
#include <functional>
#include <span>
#include <string_view>

namespace marketlib::networking {

using MessageHandler = std::function<void(std::span<const std::byte>)>;

struct ITransport {
    virtual ~ITransport() = default;
    virtual void start(MessageHandler handler)         = 0;
    virtual void stop()                                = 0;
    virtual bool send(std::span<const std::byte> data) = 0;
    virtual std::string_view name() const noexcept     = 0;
};

} // namespace marketlib::networking

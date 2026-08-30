#pragma once
#include <string>
#include <type_traits>

#include "binding/oci_compat.h"

namespace binding {

class OciClob {
public:
    OCILobLocator* locator = nullptr;
    std::string text_data;

    OciClob() = default;
    explicit OciClob(std::string data) : text_data(std::move(data)) {}
};

class OciXml {
public:
    OCILobLocator* locator = nullptr;
    std::string xml_data;

    OciXml() = default;
    explicit OciXml(std::string data) : xml_data(std::move(data)) {}
};

template <typename T>
inline constexpr bool is_oci_lob_v = std::is_same_v<T, OciClob> || std::is_same_v<T, OciXml>;

} // namespace binding

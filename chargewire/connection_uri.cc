// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "driver_profile.h"
#include <boost/url.hpp>
#include <boost/algorithm/string.hpp>

namespace driver::chargewire_odbc {
namespace {
[[noreturn]] void Invalid(const char* reason) {
  // Never repeat a URI: malformed input can contain passwords or tokens.
  throw odbcabstraction::DriverException(std::string("Invalid connection URI: ") + reason, "08001");
}
bool TlsValue(std::string value) {
  boost::algorithm::to_lower(value);
  if (value == "true" || value == "1" || value == "t" || value == "yes" || value == "on") return true;
  if (value == "false" || value == "0" || value == "f" || value == "no" || value == "off") return false;
  Invalid("tls must be true or false");
}
}
ConnectionProperties NormalizeConnectionProperties(const ConnectionProperties& properties) {
  auto out = properties;
  auto it = out.find("uri");
  if (it == out.end()) return out;
  const auto translated = TranslateProductUri(it->second);
  auto parsed = boost::urls::parse_uri(translated);
  if (!parsed) Invalid("malformed URI");
  const auto& u = *parsed;
  auto scheme = boost::algorithm::to_lower_copy(std::string(u.scheme()));
  if (scheme != "chargewire" && scheme != "https" && scheme != "http")
    Invalid("use chargewire://, https:// or http://");
  if (u.has_userinfo()) Invalid("credentials belong in uid/pwd or token properties");
  if (u.host().empty()) Invalid("a host is required");
  if (u.has_fragment()) Invalid("fragments are not supported");
  bool tls = scheme != "http";
  bool saw_tls = false;
  for (auto param : u.params()) {
    if (param.key != "tls" || scheme != "chargewire") Invalid("unknown URI parameter; only chargewire:// supports ?tls=false");
    if (saw_tls || !param.has_value) Invalid("tls must appear once with a value");
    saw_tls = true;
    tls = TlsValue(std::string(param.value));
  }
  std::string port = u.has_port() ? std::string(u.port()) : (tls ? "443" : "80");
  if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos ||
      port.size() > 5 || std::stoul(port) == 0 || std::stoul(port) > 65535) Invalid("port must be from 1 to 65535");
  std::string path(u.encoded_path());
  while (!path.empty() && path.back() == '/') path.pop_back();
  if (path.empty()) path = "/chargewire";
  out.try_emplace("host", std::string(u.host_address()));
  out.try_emplace("port", port);
  out.try_emplace("useEncryption", tls ? "true" : "false");
  out.try_emplace("basePath", path);
  return out;
}
}

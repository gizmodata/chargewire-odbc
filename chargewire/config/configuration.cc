#include "driver_profile.h"
/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "config/configuration.h"

#include "chargewire_connection.h"
#include <sstream>
#include <iterator>
#include <odbcinst.h>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>

namespace driver {
namespace chargewire_odbc {
namespace config {

static const std::string DEFAULT_DSN = GetDriverProfile().product;
static const std::string DEFAULT_ENABLE_ENCRYPTION = TRUE_STR;
static const std::string DEFAULT_USE_CERT_STORE = TRUE_STR;
static const std::string DEFAULT_DISABLE_CERT_VERIFICATION = FALSE_STR;

namespace {
std::string ReadDsnString(const std::string& dsn, const std::string& key, const std::string& dflt = "")
{
    #define BUFFER_SIZE (1024)
    std::vector<char> buf(BUFFER_SIZE);

    int ret = SQLGetPrivateProfileString(dsn.c_str(), key.c_str(), dflt.c_str(), buf.data(), static_cast<int>(buf.size()), "ODBC.INI");

    if (ret > BUFFER_SIZE)
    {
        // If there wasn't enough space, try again with the right size buffer.
        buf.resize(ret + 1);
        ret = SQLGetPrivateProfileString(dsn.c_str(), key.c_str(), dflt.c_str(), buf.data(), static_cast<int>(buf.size()), "ODBC.INI");
    }

    return std::string(buf.data(), ret);
}

void RemoveAllKnownKeys(std::vector<std::string>& keys) {
    // Remove all known DSN keys from the passed in set of keys, case insensitively.
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&](auto& x) {
        return std::find_if(ChargewireConnection::ALL_KEYS.begin(), ChargewireConnection::ALL_KEYS.end(), [&](auto& s) {
            return boost::iequals(x, s);}) != ChargewireConnection::ALL_KEYS.end();
        }), keys.end());
}

std::vector<std::string> ReadAllKeys(const std::string& dsn)
{
    std::vector<char> buf(BUFFER_SIZE);

    int ret = SQLGetPrivateProfileString(dsn.c_str(), NULL, "", buf.data(), static_cast<int>(buf.size()), "ODBC.INI");

    if (ret > BUFFER_SIZE)
    {
        // If there wasn't enough space, try again with the right size buffer.
        buf.resize(ret + 1);
        ret = SQLGetPrivateProfileString(dsn.c_str(), NULL, "", buf.data(), static_cast<int>(buf.size()), "ODBC.INI");
    }

    // When you pass NULL to SQLGetPrivateProfileString it gives back a \0 delimited list of all the keys.
    // The below loop simply tokenizes all the keys and places them into a vector.
    std::vector<std::string> keys;
    char* begin = buf.data();
    while (begin && *begin != '\0') {
        char* cur;
        for (cur = begin; *cur != '\0'; ++cur);
        keys.emplace_back(begin, cur);
        begin = ++cur;
    }
    return keys;
}
}

Configuration::Configuration()
{
    // No-op.
}

Configuration::~Configuration()
{
    // No-op.
}

void Configuration::LoadDefaults()
{
    Set(ChargewireConnection::DSN, DEFAULT_DSN);
    Set(ChargewireConnection::USE_ENCRYPTION, DEFAULT_ENABLE_ENCRYPTION);
    Set(ChargewireConnection::USE_SYSTEM_TRUST_STORE, DEFAULT_USE_CERT_STORE);
    Set(ChargewireConnection::DISABLE_CERTIFICATE_VERIFICATION, DEFAULT_DISABLE_CERT_VERIFICATION);
}

void Configuration::LoadDsn(const std::string& dsn)
{
    Set(ChargewireConnection::DSN, dsn);
    const auto uri = ReadDsnString(dsn, "uri");
    Set("uri", uri);
    Set(ChargewireConnection::AUTH_TYPE, ReadDsnString(dsn, ChargewireConnection::AUTH_TYPE));
    Set(ChargewireConnection::HOST, ReadDsnString(dsn, ChargewireConnection::HOST));
    Set(ChargewireConnection::PORT, ReadDsnString(dsn, ChargewireConnection::PORT));
    Set(ChargewireConnection::TOKEN, ReadDsnString(dsn, ChargewireConnection::TOKEN));
    Set(ChargewireConnection::UID, ReadDsnString(dsn, ChargewireConnection::UID));
    Set(ChargewireConnection::PWD, ReadDsnString(dsn, ChargewireConnection::PWD));
    Set(ChargewireConnection::USE_ENCRYPTION, 
        ReadDsnString(dsn, ChargewireConnection::USE_ENCRYPTION, uri.empty() ? DEFAULT_ENABLE_ENCRYPTION : ""));
    Set(ChargewireConnection::TRUSTED_CERTS, ReadDsnString(dsn, ChargewireConnection::TRUSTED_CERTS));
    Set(ChargewireConnection::USE_SYSTEM_TRUST_STORE, 
        ReadDsnString(dsn, ChargewireConnection::USE_SYSTEM_TRUST_STORE, DEFAULT_USE_CERT_STORE));
    Set(ChargewireConnection::DISABLE_CERTIFICATE_VERIFICATION,
        ReadDsnString(dsn, ChargewireConnection::DISABLE_CERTIFICATE_VERIFICATION, DEFAULT_DISABLE_CERT_VERIFICATION));
    Set(ChargewireConnection::DEFAULT_CATALOG, ReadDsnString(dsn, ChargewireConnection::DEFAULT_CATALOG));

    auto customKeys = ReadAllKeys(dsn);
    RemoveAllKnownKeys(customKeys);
    for (auto key : customKeys) {
        Set(key, ReadDsnString(dsn, key));
    }
}

void Configuration::Clear()
{
    this->properties.clear();
}

bool Configuration::IsSet(const std::string& key) const
{
    return 0 != this->properties.count(key);
}

const std::string& Configuration::Get(const std::string& key) const
{
    const auto itr = this->properties.find(key);
    if (itr == this->properties.cend()) {
        static const std::string empty("");
        return empty;
    }
    return itr->second;
}

void Configuration::Set(const std::string& key, const std::string& value)
{
    const std::string copy = boost::trim_copy(value);
    if (!copy.empty()) {
        this->properties[key] = value;
    }
}

const driver::odbcabstraction::Connection::ConnPropertyMap& Configuration::GetProperties() const
{
    return this->properties;
}

std::vector<std::string> Configuration::GetCustomKeys() const
{
    driver::odbcabstraction::Connection::ConnPropertyMap copyProps(properties);
    for (auto& key : ChargewireConnection::ALL_KEYS) {
        copyProps.erase(key);
    }
    std::vector<std::string> keys;
    boost::copy(copyProps | boost::adaptors::map_keys, std::back_inserter(keys));
    return keys;
}

} // namespace config
} // namespace chargewire
} // namespace driver

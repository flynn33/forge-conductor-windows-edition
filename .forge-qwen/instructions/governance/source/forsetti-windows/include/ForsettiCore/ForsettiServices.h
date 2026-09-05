// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <future>
#include <cstdint>
#include <cstddef>
#include <memory>

namespace Forsetti {

class INetworkingService {
public:
    virtual std::future<std::vector<uint8_t>> data(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}) = 0;

    virtual ~INetworkingService() = default;
};

class IStorageService {
public:
    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual void remove(const std::string& key) = 0;

    virtual ~IStorageService() = default;
};

class ISecureStorageService {
public:
    virtual void set(const std::string& key, const std::vector<uint8_t>& data) = 0;
    virtual std::optional<std::vector<uint8_t>> get(const std::string& key) = 0;
    virtual void remove(const std::string& key) = 0;

    virtual ~ISecureStorageService() = default;
};

class IFileExportService {
public:
    virtual bool exportData(const std::vector<uint8_t>& data,
                            const std::string& filename) = 0;

    virtual ~IFileExportService() = default;
};

class ITelemetryService {
public:
    virtual void trackEvent(const std::string& name,
                            const std::map<std::string, std::string>& properties = {}) = 0;

    virtual ~ITelemetryService() = default;
};

class ISharedDatabaseService {
public:
    virtual void execute(const std::string& operation,
                         const std::map<std::string, std::string>& parameters = {}) = 0;

    virtual ~ISharedDatabaseService() = default;
};

class IAuthenticationService {
public:
    virtual std::optional<std::string> currentPrincipalID() = 0;

    virtual ~IAuthenticationService() = default;
};

class IDiagnosticsService {
public:
    virtual void recordDiagnostic(const std::string& name,
                                  const std::map<std::string, std::string>& properties = {}) = 0;

    virtual ~IDiagnosticsService() = default;
};

class IApiService {
public:
    virtual std::future<std::vector<uint8_t>> invoke(
        const std::string& endpoint,
        const std::vector<uint8_t>& body = {},
        const std::map<std::string, std::string>& headers = {}) = 0;

    virtual ~IApiService() = default;
};

class ISecurityService {
public:
    virtual bool authorize(const std::string& operation,
                           const std::map<std::string, std::string>& context = {}) = 0;

    virtual ~ISecurityService() = default;
};

class ICryptoUtilitiesService {
public:
    virtual std::vector<uint8_t> randomBytes(std::size_t count) = 0;

    virtual ~ICryptoUtilitiesService() = default;
};

} // namespace Forsetti

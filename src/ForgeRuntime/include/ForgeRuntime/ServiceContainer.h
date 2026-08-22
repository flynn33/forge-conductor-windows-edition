// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace Forge::Runtime {

class IServiceProvider {
public:
    virtual void registerService(const std::type_index& type, std::shared_ptr<void> instance) = 0;
    virtual std::shared_ptr<void> resolve(const std::type_index& type) const = 0;
    virtual ~IServiceProvider() = default;

    template <typename T>
    void add(std::shared_ptr<T> instance) {
        registerService(std::type_index(typeid(T)), std::static_pointer_cast<void>(std::move(instance)));
    }

    template <typename T>
    std::shared_ptr<T> get() const {
        return std::static_pointer_cast<T>(resolve(std::type_index(typeid(T))));
    }
};

class ServiceContainer final : public IServiceProvider {
public:
    void registerService(const std::type_index& type, std::shared_ptr<void> instance) override;
    std::shared_ptr<void> resolve(const std::type_index& type) const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> services_;
};

} // namespace Forge::Runtime

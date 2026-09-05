#pragma once
#include <functional>
#include <memory>
namespace forge { template<typename T> class ILatestValueMailbox { public: virtual ~ILatestValueMailbox()=default; virtual void publish(std::shared_ptr<T const>)=0; virtual void setConsumer(std::function<void(std::shared_ptr<T const>)>)=0; virtual std::size_t pendingCount() const noexcept=0; virtual void shutdown() noexcept=0;}; }

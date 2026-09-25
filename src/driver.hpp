#pragma once
#include <cua_driver_abi.h>
#include <nlohmann/json.hpp>
#include <asio.hpp>
#include <memory>
#include <utility>
#include <stdexcept>
#include <string>

class Driver {
    struct Buffer {
        CuaDriverBuffer value{};
        ~Buffer() { cua_driver_buffer_free_v1(&value); }
        std::string text() const {
            return value.len ? std::string(reinterpret_cast<char*>(value.data), value.len) : "";
        }
    };
    CuaDriverHandle* handle_ = nullptr;
    template<class Handler> struct Pending {
        Handler handler;
        asio::any_io_executor executor;
        asio::executor_work_guard<asio::any_io_executor> work;
        CuaDriverOperation* operation = nullptr;
        Pending(Handler h, asio::any_io_executor ex)
            : handler(std::move(h)), executor(ex), work(ex) {}
        ~Pending() { cua_driver_operation_release_v1(&operation); }
        static void complete(void* context, CuaDriverStatus status,
                             CuaDriverBuffer result, CuaDriverBuffer error) noexcept {
            // The ABI owns this reference until its exactly-once callback.
            std::unique_ptr<std::shared_ptr<Pending>> owner(
                static_cast<std::shared_ptr<Pending>*>(context));
            auto state = *owner;
            Buffer data{result}, failure{error};
            std::exception_ptr exception;
            std::string text;
            try {
                if (status != CUA_DRIVER_STATUS_OK)
                    throw std::runtime_error(failure.text());
                text = data.text();
            } catch (...) { exception = std::current_exception(); }
            // Never resume the coroutine on a Rust runtime thread.
            asio::post(state->executor,
                [state, exception, text = std::move(text)]() mutable {
                    std::move(state->handler)(exception, std::move(text));
                });
        }
    };
    template<class Start> asio::awaitable<std::string> await(Start start) {
        auto executor = co_await asio::this_coro::executor;
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token),
            void(std::exception_ptr, std::string)>(
            [start = std::move(start), executor](auto handler) mutable {
                using State = Pending<decltype(handler)>;
                auto state = std::make_shared<State>(std::move(handler), executor);
                auto context = std::make_unique<std::shared_ptr<State>>(state);
                Buffer error;
                auto status = start(context.get(), &State::complete, &state->operation, &error.value);
                if (status == CUA_DRIVER_STATUS_OK) {
                    context.release(); // Callback may already have run; never dereference it here.
                } else {
                    // Rejected initiation has no callback.
                    auto exception = std::make_exception_ptr(std::runtime_error(error.text()));
                    asio::post(executor, [state, exception]() mutable {
                        std::move(state->handler)(exception, std::string{});
                    });
                }
            }, token);
    }
public:
    Driver() {
        if (!cua_driver_abi_is_compatible_v1(CUA_DRIVER_ABI_MAJOR, CUA_DRIVER_ABI_MINOR))
            throw std::runtime_error("Incompatible Cua Driver C ABI");
        Buffer error;
        if (cua_driver_create_v1(nullptr, 0, &handle_, &error.value) != CUA_DRIVER_STATUS_OK)
            throw std::runtime_error("Cua initialization failed: " + error.text());
    }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    ~Driver() {
        cua_driver_destroy_v1(&handle_);
    }
    asio::awaitable<void> shutdown() {
        co_await await([&](void* context, auto callback, auto operation, auto error) {
            return cua_driver_shutdown_v1(handle_, callback, context, operation, error);
        });
    }
    asio::awaitable<nlohmann::json> call(std::string name, nlohmann::json args) {
        auto input = args.dump();
        auto raw = co_await await([&](void* context, auto callback, auto operation, auto error) {
            return cua_driver_invoke_v1(handle_, reinterpret_cast<const uint8_t*>(name.data()), name.size(),
                reinterpret_cast<const uint8_t*>(input.data()), input.size(), callback, context, operation, error);
        });
        auto result = nlohmann::json::parse(raw);
        if (result.value("isError", false) || result.value("is_error", false))
            throw std::runtime_error(name + ": " + result.dump());
        if (result.contains("structuredContent") && result["structuredContent"].is_object())
            co_return result["structuredContent"];
        co_return result;
    }
};

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <functional>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra::Host1x {

struct SyncpointManagerTestAccess {
    // Registration rejects empty actions. Seed both queues to exercise the dispatch safeguard.
    static void QueueEmptyCallbacks(SyncpointManager& manager) {
        manager.guest_action_storage[0].emplace_back(1, ++manager.last_action_id, std::function<void()>{});
        manager.host_action_storage[0].emplace_back(1, ++manager.last_action_id, std::function<void()>{});
    }
};

} // namespace Tegra::Host1x

TEST_CASE("SyncpointManager preserves reusable callbacks", "[video_core][syncpoint]") {
    Tegra::Host1x::SyncpointManager manager;
    const bool host = GENERATE(false, true);
    auto register_action = [&](u32 value, auto&& action) {
        if (host) {
            return manager.RegisterHostAction(0, value, std::forward<decltype(action)>(action));
        }
        return manager.RegisterGuestAction(0, value, std::forward<decltype(action)>(action));
    };
    auto increment = [&] {
        if (host) {
            manager.IncrementHost(0);
        } else {
            manager.IncrementGuest(0);
        }
    };

    int calls = 0;
    std::function<void()> action = [&] { ++calls; };
    register_action(2, action);
    REQUIRE(static_cast<bool>(action));
    register_action(1, action);
    REQUIRE(static_cast<bool>(action));
    increment();
    REQUIRE(calls == 1);
    increment();
    REQUIRE(calls == 2);
    increment();
    REQUIRE(calls == 2);
    register_action(2, action);
    REQUIRE(calls == 3);
    REQUIRE(static_cast<bool>(action));
}

TEST_CASE("SyncpointManager ignores empty callbacks", "[video_core][syncpoint]") {
    Tegra::Host1x::SyncpointManager manager;
    const bool host = GENERATE(false, true);
    auto register_action = [&](u32 value, std::function<void()>&& action) {
        if (host) {
            return manager.RegisterHostAction(0, value, std::move(action));
        }
        return manager.RegisterGuestAction(0, value, std::move(action));
    };
    auto increment = [&] {
        if (host) {
            manager.IncrementHost(0);
        } else {
            manager.IncrementGuest(0);
        }
    };

    REQUIRE_NOTHROW(register_action(0, {}));
    REQUIRE_NOTHROW(register_action(1, {}));
    int calls = 0;
    std::function<void()> action = [&] { ++calls; };
    register_action(1, std::move(action));
    action = {};
    REQUIRE_NOTHROW(register_action(1, std::move(action)));
    increment();
    REQUIRE(calls == 1);
    increment();
    REQUIRE(calls == 1);
    REQUIRE_NOTHROW(register_action(1, {}));
}

TEST_CASE("SyncpointManager retires empty stored callbacks", "[video_core][syncpoint]") {
    Tegra::Host1x::SyncpointManager manager;
    int calls = 0;
    Tegra::Host1x::SyncpointManagerTestAccess::QueueEmptyCallbacks(manager);
    manager.RegisterGuestAction(0, 1, [&] { ++calls; });
    manager.RegisterHostAction(0, 1, [&] { ++calls; });
    REQUIRE_NOTHROW(manager.IncrementGuest(0));
    REQUIRE_NOTHROW(manager.IncrementHost(0));
    REQUIRE(calls == 2);
    manager.IncrementGuest(0);
    manager.IncrementHost(0);
    REQUIRE(calls == 2);
}

TEST_CASE("SyncpointManager cancels pending callbacks", "[video_core][syncpoint]") {
    Tegra::Host1x::SyncpointManager manager;
    int calls = 0;
    const auto guest = manager.RegisterGuestAction(0, 1, [&] { ++calls; });
    const auto host = manager.RegisterHostAction(0, 1, [&] { ++calls; });
    manager.DeregisterGuestAction(0, guest);
    manager.DeregisterHostAction(0, host);
    manager.IncrementGuest(0);
    manager.IncrementHost(0);
    REQUIRE(calls == 0);
    REQUIRE(manager.GetGuestSyncpointValue(0) == 1);
    REQUIRE(manager.GetHostSyncpointValue(0) == 1);
}

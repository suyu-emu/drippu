// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

// Catch2's default console reporter prints the case name lazily (after the
// first assertion, or at the end). Hang-prone cases like Fibers::InterExchange
// join() before REQUIRE, so -s still shows only the seed line. Name the case
// at start and flush so a timeout log shows which test was running.
class CatchStartingListener : public Catch::EventListenerBase {
public:
    using EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const& test_info) override {
        std::cout << "Starting test case: " << test_info.name << std::endl;
        std::cout.flush();
    }
};

CATCH_REGISTER_LISTENER(CatchStartingListener)

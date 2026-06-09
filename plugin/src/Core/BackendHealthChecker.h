#pragma once

/**
 * BackendHealthChecker — Periodic background health-check against the Python backend.
 *
 * Polls GET http://127.0.0.1:5000/health on a background thread every N seconds.
 * Exposes an atomic BackendState that the UI timer can read without blocking.
 *
 * Three operator-visible states:
 *   Unreachable      — cannot connect to 127.0.0.1:5000
 *   NoInventory      — connected but inventory unavailable (rescan required)
 *   Ready            — connected and inventory available
 *
 * Thread safety:
 *   - HTTP work runs on a member thread pool (never on audio or message thread).
 *   - State is exposed via std::atomic<int>, safe for lock-free UI reads.
 *   - Timer fires on the message thread, submits work to the pool.
 *   - Destructor stops the timer, then the member pool destructor waits for
 *     any in-flight job to complete — no dangling references.
 */

#include <JuceHeader.h>

enum class BackendState : int
{
    Unknown       = 0,  // Haven't checked yet
    Unreachable   = 1,  // Can't connect to 127.0.0.1:5000
    NoInventory   = 2,  // Connected but inventory unavailable
    Ready         = 3   // Connected and inventory available
};

class BackendHealthChecker
{
public:
    ~BackendHealthChecker()
    {
        stopChecking();
        // Member pool destructor waits for in-flight jobs — no dangling state& references.
    }

    // ── Timer control ──
    void startChecking (int intervalMs = 10000)
    {
        checkTimer.checker = this;
        checkTimer.startTimer (intervalMs);
    }

    void stopChecking()
    {
        checkTimer.stopTimer();
    }

    /** Trigger an immediate check (e.g. after rescan).
        Safe to call from any thread — submits a job to the member pool. */
    void checkNow()
    {
        submitHealthCheck();
    }

    /** Current backend state. Read from any thread. */
    std::atomic<int> state { static_cast<int> (BackendState::Unknown) };

    BackendState getState() const noexcept
    {
        return static_cast<BackendState> (state.load (std::memory_order_relaxed));
    }

private:
    void submitHealthCheck()
    {
        struct HealthCheckJob : public juce::ThreadPoolJob
        {
            std::atomic<int>& targetState;

            explicit HealthCheckJob (std::atomic<int>& s)
                : ThreadPoolJob ("health-check"), targetState (s) {}

            JobStatus runJob() override
            {
                juce::URL healthURL ("http://127.0.0.1:5000/health");

                int statusCode = 0;
                auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                .withConnectionTimeoutMs (5000)
                                .withHttpRequestCmd ("GET")
                                .withStatusCode (&statusCode);

                auto stream = healthURL.createInputStream (opts);

                if (stream == nullptr || statusCode < 200 || statusCode >= 300)
                {
                    targetState.store (static_cast<int> (BackendState::Unreachable),
                                       std::memory_order_relaxed);
                    return jobHasFinished;
                }

                juce::String body = stream->readEntireStreamAsString();
                auto json = juce::JSON::parse (body);

                if (! json.isObject())
                {
                    targetState.store (static_cast<int> (BackendState::Unreachable),
                                       std::memory_order_relaxed);
                    return jobHasFinished;
                }

                // Check inventory.available from the /health response
                auto inventory = json.getProperty ("inventory", juce::var());
                bool inventoryAvailable = false;

                if (inventory.isObject())
                    inventoryAvailable = static_cast<bool> (inventory.getProperty ("available", false));

                if (inventoryAvailable)
                    targetState.store (static_cast<int> (BackendState::Ready),
                                       std::memory_order_relaxed);
                else
                    targetState.store (static_cast<int> (BackendState::NoInventory),
                                       std::memory_order_relaxed);

                return jobHasFinished;
            }
        };

        healthPool.addJob (new HealthCheckJob (state), true);
    }

    // Member pool: destructor waits for in-flight jobs, preventing dangling references.
    juce::ThreadPool healthPool { 1 };

    struct CheckTimer : public juce::Timer
    {
        BackendHealthChecker* checker = nullptr;
        void timerCallback() override
        {
            if (checker != nullptr)
                checker->submitHealthCheck();
        }
    };
    CheckTimer checkTimer;
};

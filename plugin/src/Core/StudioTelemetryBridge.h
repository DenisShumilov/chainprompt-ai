#pragma once

/**
 * StudioTelemetryBridge — Background ping timer + payload assembly.
 *
 * Extracted from ChainPromptAudioProcessor (Sprint 11: Great Decoupling, Step 3).
 * Owns the PingTimer and HTTP fire-and-forget logic. Identity strings and metrics
 * are passed explicitly — no circular dependency on the processor.
 *
 * Session scope contract: the scope token is an opaque string. Default is PID-based
 * (all instances in the same DAW process share one scope). Future Master Mode can
 * override with a real project hash. The bridge does NOT hardcode PID semantics.
 *
 * Usage:
 *   bridge.onTick = [&]() { detectTouches(); bridge.sendPing(...); };
 *   bridge.startPinging (15000);
 */

#include <JuceHeader.h>
#include "PresetData.h"
#include "AudioMetrics.h"

class StudioTelemetryBridge
{
public:
    ~StudioTelemetryBridge() { stopPinging(); }

    // ── Timer control ──
    void startPinging (int intervalMs = 15000)
    {
        pingTimer.bridge = this;
        pingTimer.startTimer (intervalMs);
    }

    void stopPinging()
    {
        pingTimer.stopTimer();
    }

    /** Called on each timer tick. Owner sets this to run detectUserTouches + sendPing. */
    std::function<void()> onTick;

    // ── Payload assembly: builds JSON string from explicit data ──
    static juce::String buildPayload (
        const juce::String& instanceId,
        const juce::String& trackName,
        const juce::String& sessionScope,
        const ChainPreset& currentPreset,
        const AudioMetrics& metrics)
    {
        auto* body = new juce::DynamicObject();
        body->setProperty ("instance_id", instanceId);
        body->setProperty ("track_name", trackName);
        body->setProperty ("project_scope", sessionScope);

        // Current chain: full slot contract
        {
            juce::Array<juce::var> chainArr;
            for (int i = 0; i < currentPreset.numPlugins; ++i)
            {
                auto si = static_cast<size_t> (i);
                auto* entry = new juce::DynamicObject();
                entry->setProperty ("uid", currentPreset.plugins[si].uid);
                entry->setProperty ("slot_id", currentPreset.plugins[si].slotId);
                entry->setProperty ("user_touched", currentPreset.userTouched[si]);
                entry->setProperty ("bypassed", currentPreset.bypassed[si]);
                entry->setProperty ("user_owned", currentPreset.userOwned[si]);
                chainArr.add (juce::var (entry));
            }
            body->setProperty ("current_chain", chainArr);
        }

        // Full metric set
        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("rmsDB", static_cast<double> (metrics.rmsDB.load (std::memory_order_relaxed)));
            m->setProperty ("peakDB", static_cast<double> (metrics.peakDB.load (std::memory_order_relaxed)));
            m->setProperty ("crestFactor", static_cast<double> (metrics.crestFactor.load (std::memory_order_relaxed)));
            m->setProperty ("zcr", static_cast<double> (metrics.zcr.load (std::memory_order_relaxed)));
            m->setProperty ("bpm", metrics.bpm.load (std::memory_order_relaxed));
            m->setProperty ("subPct", static_cast<double> (metrics.subPct.load (std::memory_order_relaxed)));
            m->setProperty ("lowPct", static_cast<double> (metrics.lowPct.load (std::memory_order_relaxed)));
            m->setProperty ("lowMidPct", static_cast<double> (metrics.lowMidPct.load (std::memory_order_relaxed)));
            m->setProperty ("highMidPct", static_cast<double> (metrics.highMidPct.load (std::memory_order_relaxed)));
            m->setProperty ("airPct", static_cast<double> (metrics.airPct.load (std::memory_order_relaxed)));
            m->setProperty ("isStereo", metrics.isStereo.load (std::memory_order_relaxed));

            m->setProperty ("mdTserDb", static_cast<double> (metrics.mdTserDb.load (std::memory_order_relaxed)));
            m->setProperty ("spectralFlux", static_cast<double> (metrics.spectralFlux.load (std::memory_order_relaxed)));
            m->setProperty ("oeHerDb", static_cast<double> (metrics.oeHerDb.load (std::memory_order_relaxed)));
            m->setProperty ("isTonal", metrics.isTonal.load (std::memory_order_relaxed));

            // BW-ICC: 4-band inter-channel coherence
            {
                juce::Array<juce::var> iccArr;
                for (int i = 0; i < 4; ++i)
                    iccArr.add (juce::var (static_cast<double> (
                        metrics.bwIcc[static_cast<size_t> (i)].load (std::memory_order_relaxed))));
                m->setProperty ("bwIcc", iccArr);
            }

            m->setProperty ("lrBalanceDb", static_cast<double> (metrics.lrBalanceDb.load (std::memory_order_relaxed)));

            // 31-band spectrum if available
            if (metrics.spectrum31Ready.load (std::memory_order_relaxed))
            {
                juce::Array<juce::var> specArr;
                for (int i = 0; i < 31; ++i)
                    specArr.add (juce::var (static_cast<double> (
                        metrics.spectrum31[static_cast<size_t> (i)].load (std::memory_order_relaxed))));
                m->setProperty ("spectrum31", specArr);
            }

            body->setProperty ("metrics", juce::var (m));
        }

        return juce::JSON::toString (juce::var (body));
    }

    // ── Fire-and-forget HTTP POST ──
    static void sendPayloadAsync (juce::String jsonPayload)
    {
        juce::URL pingURL ("http://127.0.0.1:5000/api/ping");
        pingURL = pingURL.withPOSTData (jsonPayload);

        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs (3000)
                           .withExtraHeaders ("Content-Type: application/json")
                           .withHttpRequestCmd ("POST")
                           .withStatusCode (&statusCode);

        struct PingTask : public juce::ThreadPoolJob
        {
            juce::URL url;
            juce::URL::InputStreamOptions opts;

            PingTask (juce::URL u, juce::URL::InputStreamOptions o)
                : ThreadPoolJob ("ping"), url (std::move (u)), opts (std::move (o)) {}

            JobStatus runJob() override
            {
                url.createInputStream (opts);
                return jobHasFinished;
            }
        };

        static juce::ThreadPool pingPool (1);
        pingPool.addJob (new PingTask (std::move (pingURL), std::move (options)), true);
    }

private:
    struct PingTimer : public juce::Timer
    {
        StudioTelemetryBridge* bridge = nullptr;
        void timerCallback() override
        {
            if (bridge != nullptr && bridge->onTick)
                bridge->onTick();
        }
    };
    PingTimer pingTimer;
};

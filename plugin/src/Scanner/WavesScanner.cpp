/*
    WavesScanner — TRUE 1001-point VST3 Parameter Dumper
    =====================================================
    Loads every Waves VST3 plugin, calls getText() at 1001 normalized
    positions (0.000 → 1.000, step 0.001) for every parameter, and
    writes the GROUND-TRUTH physical values to a provider-neutral
    metadata envelope (Waves_HighRes_DB.json).

    Envelope contract (shared by all future extractors):
      { "provider": "<id>", "provider_version": <int>,
        "scan_timestamp": "<ISO-8601>", "plugins": [...] }

    No PCHIP.  No interpolation.  No guessing.  Pure API dump.
*/

#include <JuceHeader.h>
#include <fstream>
#include <regex>
#include <ctime>
#include <iomanip>

// ─── Configuration ───────────────────────────────────────────────────────────
static constexpr int    NUM_POINTS        = 1001;      // 0.000 → 1.000, step 0.001
static constexpr int    MAX_TEXT_LEN      = 256;
static const char*      OUTPUT_FILE       = "Waves_HighRes_DB.json";
static const char*      PROVIDER_ID       = "waves_truescan";
static constexpr int    PROVIDER_VERSION  = 1;

// ─── Numeric extraction ──────────────────────────────────────────────────────
// Tries to parse a float + optional unit from plugin display text like
// "578 Hz", "-3.5 dB", "45.0%", "1.30", "Off".
// Returns {true, value, unit} or {false, 0, ""}.
struct ParseResult { bool ok; float value; juce::String unit; };

static ParseResult tryParseFloat (const juce::String& text)
{
    auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return { false, 0.0f, {} };

    // Reject if starts with letter (discrete label like "Off", "Bell")
    if (juce::CharacterFunctions::isLetter (trimmed[0])
        && trimmed[0] != '-' && trimmed[0] != '+')
        return { false, 0.0f, {} };

    // Reject ratios like "4:1", inf
    if (trimmed.contains (":") && ! trimmed.contains ("dB"))
        return { false, 0.0f, {} };
    if (trimmed.toLowerCase() == "inf" || trimmed.toLowerCase() == "-inf")
        return { false, 0.0f, {} };

    // Use regex: optional sign, digits, optional decimal, optional unit
    static const std::regex re (R"(^([+-]?\d+(?:\.\d+)?)\s*(%|[a-zA-Z/°]+)?\s*$)");
    std::string s = trimmed.toStdString();
    std::smatch m;
    if (std::regex_match (s, m, re))
    {
        float val = std::stof (m[1].str());
        juce::String unit = m[2].matched ? juce::String (m[2].str()) : juce::String();
        return { true, val, unit };
    }

    return { false, 0.0f, {} };
}

// ─── JSON writing helpers (manual, to avoid juce::var overhead) ──────────────
static juce::String escapeJson (const juce::String& s)
{
    return s.replace ("\\", "\\\\")
            .replace ("\"", "\\\"")
            .replace ("\n", "\\n")
            .replace ("\r", "\\r")
            .replace ("\t", "\\t");
}

// ─── Main scanner ────────────────────────────────────────────────────────────
class WavesScannerApp : public juce::JUCEApplicationBase
{
public:
    const juce::String getApplicationName() override    { return "WavesScanner"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        juce::MessageManager::callAsync ([this]() { runScan(); });
    }

    void shutdown() override {}
    void anotherInstanceStarted (const juce::String&) override {}
    void systemRequestedQuit() override { quit(); }
    void suspended() override {}
    void resumed() override {}
    void unhandledException (const std::exception*, const juce::String&, int) override {}

private:
    void runScan()
    {
        // Force unbuffered output so we can see progress in real-time
        std::cout.setf (std::ios::unitbuf);
        std::cerr.setf (std::ios::unitbuf);

        // Scanner log goes to %APPDATA%/ChainPrompt/ (operational output)
        juce::File appDataDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                    .getChildFile ("ChainPrompt");
        appDataDir.createDirectory();
        juce::File logFile = appDataDir.getChildFile ("scanner_log.txt");
        std::ofstream logOut (logFile.getFullPathName().toStdString());

        auto logMsg = [&](const std::string& msg) {
            std::cout << msg << std::endl;
            if (logOut.is_open()) { logOut << msg << std::endl; logOut.flush(); }
        };

        logMsg ("=== WavesScanner: TRUE 1001-point VST3 Parameter Dump ===");

        // 1. Set up plugin format manager (VST3 only)
        juce::AudioPluginFormatManager formatManager;
        formatManager.addFormat (new juce::VST3PluginFormat());

        // 2. Scan for plugins
        juce::String vst3Path = "C:\\Program Files\\Common Files\\VST3";
        juce::FileSearchPath searchPath (vst3Path);

        juce::KnownPluginList knownPlugins;
        juce::PluginDirectoryScanner scanner (
            knownPlugins,
            *formatManager.getFormat (0),  // VST3
            searchPath,
            true,   // recursive
            juce::File()  // dead plugins file
        );

        juce::String pluginName;
        logMsg ("Scanning " + vst3Path.toStdString() + " ...");
        while (scanner.scanNextFile (true, pluginName))
        {
            // silent during scan — too noisy
        }

        auto types = knownPlugins.getTypes();
        logMsg ("Total plugins found: " + std::to_string (types.size()));

        // Filter to Waves plugins only
        juce::Array<juce::PluginDescription> wavesPlugins;
        for (auto& desc : types)
        {
            if (desc.manufacturerName.containsIgnoreCase ("Waves"))
                wavesPlugins.add (desc);
        }
        logMsg ("Waves plugins: " + std::to_string (wavesPlugins.size()));

        if (wavesPlugins.isEmpty())
        {
            logMsg ("ERROR: No Waves plugins found!");
            quit();
            return;
        }

        // 3. Open output file — canonical location: repo data/catalogs/
        //    (matches server/config.py HIGHRES_DB_PATH)
#ifdef CHAINPROMPT_DATA_DIR
        juce::File catalogDir = juce::File (CHAINPROMPT_DATA_DIR).getChildFile ("catalogs");
#else
        juce::File catalogDir = appDataDir.getChildFile ("catalogs");
#endif
        if (! catalogDir.isDirectory())
            catalogDir.createDirectory();

        juce::File outFile = catalogDir.getChildFile (OUTPUT_FILE);
        logMsg ("Output target: " + outFile.getFullPathName().toStdString());
        logMsg ("Log target:    " + logFile.getFullPathName().toStdString());

        std::ofstream out (outFile.getFullPathName().toStdString(), std::ios::binary);
        if (! out.is_open())
        {
            logMsg ("ERROR: Cannot open " + outFile.getFullPathName().toStdString());
            quit();
            return;
        }

        // Write provider-neutral envelope header
        {
            auto now = std::time (nullptr);
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s (&utc, &now);
#else
            gmtime_r (&now, &utc);
#endif
            char timeBuf[64];
            std::strftime (timeBuf, sizeof (timeBuf), "%Y-%m-%dT%H:%M:%SZ", &utc);

            out << "{\"provider\":\"" << PROVIDER_ID << "\""
                << ",\"provider_version\":" << PROVIDER_VERSION
                << ",\"scan_timestamp\":\"" << timeBuf << "\""
                << ",\"plugins\":[";
        }
        bool firstPlugin = true;
        int scannedCount = 0;
        int errorCount = 0;

        // 4. Process each Waves plugin
        for (int pi = 0; pi < wavesPlugins.size(); ++pi)
        {
            auto& desc = wavesPlugins.getReference (pi);
            logMsg ("[" + std::to_string(pi + 1) + "/" + std::to_string(wavesPlugins.size())
                 + "] " + desc.name.toStdString() + " (uid:" + std::to_string(desc.uniqueId) + ") ...");

            // Create plugin instance
            juce::String errorMsg;
            auto instance = formatManager.createPluginInstance (
                desc, 44100.0, 512, errorMsg);

            if (instance == nullptr)
            {
                logMsg ("  FAILED: " + errorMsg.toStdString());
                errorCount++;
                continue;
            }

            // Prepare plugin (some plugins need this before getText works correctly)
            instance->prepareToPlay (44100.0, 512);

            auto& params = instance->getParameters();
            int paramCount = params.size();

            // Start plugin JSON object
            if (! firstPlugin) out << ",";
            firstPlugin = false;

            out << "{\"name\":\"" << escapeJson (desc.name).toStdString() << "\""
                << ",\"uid\":" << desc.uniqueId
                << ",\"manufacturer\":\"" << escapeJson (desc.manufacturerName).toStdString() << "\"";

            // Carry over category if available
            if (desc.category.isNotEmpty())
                out << ",\"category\":\"" << escapeJson (desc.category).toStdString() << "\"";

            out << ",\"parameters\":[";

            bool firstParam = true;
            int usableParams = 0;

            for (int j = 0; j < paramCount; ++j)
            {
                auto* param = params[j];
                int paramIdx = param->getParameterIndex();
                juce::String paramName = param->getName (MAX_TEXT_LEN);

                // Sample 1001 points: getText at each normalized position
                bool allBlank = true;
                bool isContinuous = false;
                juce::String detectedUnit;
                int numericCount = 0;

                struct SamplePoint { float norm; juce::String text; float physVal; bool isNumeric; };
                std::vector<SamplePoint> points;
                points.reserve (NUM_POINTS);

                for (int si = 0; si < NUM_POINTS; ++si)
                {
                    float norm = static_cast<float> (si) / static_cast<float> (NUM_POINTS - 1);
                    juce::String text = param->getText (norm, MAX_TEXT_LEN).trim();

                    SamplePoint pt;
                    pt.norm = norm;
                    pt.text = text;
                    pt.isNumeric = false;
                    pt.physVal = 0.0f;

                    if (text.isNotEmpty())
                        allBlank = false;

                    auto parsed = tryParseFloat (text);
                    if (parsed.ok)
                    {
                        pt.isNumeric = true;
                        pt.physVal = parsed.value;
                        numericCount++;
                        if (parsed.unit.isNotEmpty() && detectedUnit.isEmpty())
                            detectedUnit = parsed.unit;
                    }

                    points.push_back (pt);
                }

                if (allBlank)
                    continue;  // skip totally blank params

                // Classify: continuous if >= 80% of points are numeric
                isContinuous = (numericCount >= (NUM_POINTS * 4 / 5));

                if (! firstParam) out << ",";
                firstParam = false;

                out << "{\"index\":" << paramIdx
                    << ",\"name\":\"" << escapeJson (paramName).toStdString() << "\"";

                if (isContinuous)
                {
                    out << ",\"type\":\"continuous\""
                        << ",\"unit\":\"" << escapeJson (detectedUnit).toStdString() << "\""
                        << ",\"default\":" << param->getDefaultValue()
                        << ",\"map\":[";

                    // Write 1001 physical values as flat array
                    for (int si = 0; si < NUM_POINTS; ++si)
                    {
                        if (si > 0) out << ",";

                        if (points[static_cast<size_t>(si)].isNumeric)
                        {
                            // Round to 4 decimal places
                            float v = points[static_cast<size_t>(si)].physVal;
                            char buf[32];
                            snprintf (buf, sizeof (buf), "%.4g", static_cast<double>(v));
                            out << buf;
                        }
                        else
                        {
                            // Non-numeric point in a mostly-continuous param:
                            // interpolate from neighbors
                            float interpVal = 0.0f;
                            bool found = false;

                            // Search backward for nearest numeric
                            float prevVal = 0.0f; int prevIdx = -1;
                            for (int bi = si - 1; bi >= 0; --bi)
                            {
                                if (points[static_cast<size_t>(bi)].isNumeric)
                                {
                                    prevVal = points[static_cast<size_t>(bi)].physVal;
                                    prevIdx = bi;
                                    break;
                                }
                            }
                            // Search forward for nearest numeric
                            float nextVal = 0.0f; int nextIdx = -1;
                            for (int fi = si + 1; fi < NUM_POINTS; ++fi)
                            {
                                if (points[static_cast<size_t>(fi)].isNumeric)
                                {
                                    nextVal = points[static_cast<size_t>(fi)].physVal;
                                    nextIdx = fi;
                                    break;
                                }
                            }

                            if (prevIdx >= 0 && nextIdx >= 0)
                            {
                                float t = static_cast<float>(si - prevIdx) /
                                          static_cast<float>(nextIdx - prevIdx);
                                interpVal = prevVal + t * (nextVal - prevVal);
                                found = true;
                            }
                            else if (prevIdx >= 0) { interpVal = prevVal; found = true; }
                            else if (nextIdx >= 0) { interpVal = nextVal; found = true; }

                            if (found)
                            {
                                char buf[32];
                                snprintf (buf, sizeof (buf), "%.4g", static_cast<double>(interpVal));
                                out << buf;
                            }
                            else
                                out << "0";
                        }
                    }
                    out << "]";
                }
                else
                {
                    // Discrete parameter: collect unique labels → normalized float
                    out << ",\"type\":\"discrete\""
                        << ",\"default\":" << param->getDefaultValue()
                        << ",\"options\":{";

                    // Walk all 1001 points, record first occurrence of each label
                    std::map<juce::String, float> options;
                    for (auto& pt : points)
                    {
                        if (pt.text.isNotEmpty() && options.find (pt.text) == options.end())
                            options[pt.text] = pt.norm;
                    }

                    // Tag bypass/toggle params with (ACTIVE)/(MUTED)
                    bool firstOpt = true;
                    for (auto& [label, norm] : options)
                    {
                        if (! firstOpt) out << ",";
                        firstOpt = false;

                        juce::String tagged = tagBypassState (paramName, label, norm);
                        out << "\"" << escapeJson (tagged).toStdString()
                            << "\":" << norm;
                    }
                    out << "}";
                }

                out << "}";
                usableParams++;
            }

            out << "]}";
            scannedCount++;

            // Release plugin
            instance->releaseResources();
            instance.reset();

            logMsg ("  OK (" + std::to_string(usableParams) + " params)");
        }

        out << "]}";
        out.close();

        logMsg ("=== DONE ===");
        logMsg ("Scanned: " + std::to_string(scannedCount) + " plugins");
        logMsg ("Errors: " + std::to_string(errorCount) + " plugins failed to load");
        logMsg ("Output: " + outFile.getFullPathName().toStdString());

        quit();
    }

    // Tag bypass/toggle labels with (ACTIVE)/(MUTED)
    static juce::String tagBypassState (const juce::String& paramName,
                                         const juce::String& label,
                                         float normValue)
    {
        auto nameLower = paramName.toLowerCase();
        auto labelLower = label.toLowerCase().trim();

        // Rule A: Master Bypass (inverted logic)
        if (nameLower.contains ("bypass"))
        {
            if (labelLower == "off" || (labelLower != "on" && normValue == 0.0f))
                return label + " (ACTIVE)";
            return label + " (MUTED)";
        }

        // Rule B: Module toggles (standard logic)
        bool isToggle = nameLower.contains ("on/off")
                     || nameLower.contains ("enable")
                     || nameLower.contains ("solo");

        if (! isToggle)
        {
            // Whole-word match for "In"/"Out"
            isToggle = paramName.contains ("In") && ! paramName.contains ("Input");
            if (! isToggle)
                isToggle = paramName.contains ("Out") && ! paramName.contains ("Output");
        }

        if (isToggle)
        {
            if (labelLower == "off" || labelLower == "out"
                || (labelLower != "on" && labelLower != "in" && normValue == 0.0f))
                return label + " (MUTED)";
            return label + " (ACTIVE)";
        }

        return label;
    }
};

// ─── Entry point ─────────────────────────────────────────────────────────────
START_JUCE_APPLICATION (WavesScannerApp)

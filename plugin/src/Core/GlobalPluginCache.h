#pragma once

#include <JuceHeader.h>
#include <set>
#include <map>

/**
 * GlobalPluginCache — Thread-safe singleton shared via
 * juce::SharedResourcePointer.
 *
 * Scans ALL VST3 plugins in standard directories (vendor-neutral).
 * Uses JUCE PluginDirectoryScanner with dead-man's-pedal for crash safety.
 *
 * After scan/load, exports installed_plugins.json v2 — the single source
 * of truth for what the Python backend is allowed to route.
 *
 * Rescan notifications use removable multi-listener semantics via
 * AsyncUpdater (safe against late-firing callbacks after listener removal).
 *
 * Usage: juce::SharedResourcePointer<GlobalPluginCache> pluginCache;
 *        — first instance triggers the scan; all others reuse the result.
 */
class GlobalPluginCache : public juce::AsyncUpdater {
public:
  /** Diff result after a rescan: lists what changed. */
  struct RescanDiff {
    juce::StringArray added;    // plugins found after rescan that weren't before
    juce::StringArray removed;  // plugins that disappeared after rescan
    juce::StringArray updated;  // plugins whose version changed (re-scanned)
    int totalInstalled = 0;
    juce::String scanMode;      // "full_vst3" or "differential"
  };

  GlobalPluginCache() {
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());

    // One-time migration: delete legacy WaveShell-only cache.
    auto legacyCache = cacheFile.getParentDirectory().getChildFile("WaveShellCache.xml");
    if (legacyCache.existsAsFile()) {
      legacyCache.deleteFile();
      DBG("GlobalPluginCache: deleted legacy WaveShellCache.xml (forcing full VST3 scan)");
    }

    // Try loading persistent disk cache first
    if (cacheFile.existsAsFile()) {
      auto xml = juce::XmlDocument::parse(cacheFile);
      if (xml != nullptr) {
        knownPluginList.recreateFromXml(*xml);

        if (knownPluginList.getNumTypes() > 0) {
          for (auto &desc : knownPluginList.getTypes())
            cachedPlugins.add(new juce::PluginDescription(desc));

          DBG("GlobalPluginCache: loaded " +
              juce::String(cachedPlugins.size()) + " plugins from disk cache");

          exportInstalledManifest();
          scanComplete.store(true);

          // Fire any initial-scan listeners that were added before constructor finished
          const juce::ScopedLock sl(listenerLock);
          for (auto &entry : pendingListeners)
            juce::MessageManager::callAsync(std::move(entry.callback));
          pendingListeners.clear();

          return;
        }
      }
    }

    // No valid cache — scan all VST3 plugins from scratch
    scannerThread.startThread(juce::Thread::Priority::background);
  }

  ~GlobalPluginCache() override {
    cancelPendingUpdate();   // cancel any pending rescan notification
    scannerThread.stopThread(10000);
  }

  /** True once scanning is complete and plugin list is ready. */
  bool isReady() const noexcept { return scanComplete.load(); }

  /** True while scan is currently in progress. */
  bool isScanning() const noexcept { return !scanComplete.load(); }

  /** Thread-safe read access to cached plugin descriptions. */
  const juce::OwnedArray<juce::PluginDescription> &getPlugins() const noexcept {
    return cachedPlugins;
  }

  /** Find a plugin description by uniqueId. Thread-safe after scan completes. */
  juce::PluginDescription *findPluginByUID(int uid) const {
    if (!scanComplete.load())
      return nullptr;

    for (auto *desc : cachedPlugins)
      if (desc->uniqueId == uid)
        return desc;

    return nullptr;
  }

  /** Create a plugin instance. Must be called from the Message Thread. */
  std::unique_ptr<juce::AudioPluginInstance>
  createInstance(const juce::PluginDescription &desc, double sampleRate,
                 int blockSize, juce::String &error) {
    return formatManager.createPluginInstance(desc, sampleRate, blockSize,
                                              error);
  }

  /** Register a listener to be notified (on Message Thread) when initial
   *  scan completes. Returns an ID that can be passed to removeListener(). */
  int addListener(std::function<void()> callback) {
    const juce::ScopedLock sl(listenerLock);
    int id = nextListenerId++;
    if (scanComplete.load()) {
      juce::MessageManager::callAsync(std::move(callback));
    } else {
      pendingListeners.push_back({id, std::move(callback)});
    }
    return id;
  }

  /** Remove a previously registered initial-scan listener by ID. */
  void removeListener(int listenerId) {
    const juce::ScopedLock sl(listenerLock);
    pendingListeners.erase(
        std::remove_if(pendingListeners.begin(), pendingListeners.end(),
                        [listenerId](const ListenerEntry& e) { return e.id == listenerId; }),
        pendingListeners.end());
  }

  /** Register a listener for rescan completion (with diff results).
   *  Called on the Message Thread after rescan finishes.
   *  Returns an ID for safe removal. Multiple listeners supported. */
  int addRescanListener(std::function<void(const RescanDiff&)> callback) {
    const juce::ScopedLock sl(listenerLock);
    int id = nextListenerId++;
    rescanListeners.push_back({id, std::move(callback)});
    return id;
  }

  /** Remove a rescan listener by ID. Safe to call from any thread,
   *  but typically called from a destructor on the Message Thread. */
  void removeRescanListener(int listenerId) {
    const juce::ScopedLock sl(listenerLock);
    rescanListeners.erase(
        std::remove_if(rescanListeners.begin(), rescanListeners.end(),
                        [listenerId](const RescanListenerEntry& e) { return e.id == listenerId; }),
        rescanListeners.end());
  }

  /** Differential rescan: keeps knownPluginList so the scanner only processes
   *  new/changed files. Removed plugins are pruned after scan completes.
   *  Dead-man's pedal is cleared to retry previously-crashed plugins. */
  void requestRescan() {
    const juce::ScopedLock sl(listenerLock);
    if (!scanComplete.load() && scannerThread.isThreadRunning())
      return;  // Already scanning

    // Snapshot current state for diff after rescan
    preRescanUIDs.clear();
    preRescanNames.clear();
    preRescanVersions.clear();
    for (auto *desc : cachedPlugins)
    {
      preRescanUIDs.insert(desc->uniqueId);
      preRescanNames[desc->uniqueId] = desc->name;
      preRescanVersions[desc->uniqueId] = desc->version;
    }

    // Differential: keep knownPluginList (scanner skips known unchanged files)
    // Keep cache file (preserve scan history for faster differential)
    scanComplete.store(false);
    cachedPlugins.clear();

    // Only clear dead-man's pedal to retry previously-crashed plugins
    if (deadMansPedalFile.existsAsFile())
      deadMansPedalFile.deleteFile();

    isRescan.store(true);
    scannerThread.startThread(juce::Thread::Priority::background);
  }

  /** Path to the installed_plugins.json manifest (shared with Python). */
  static juce::File getManifestFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("ChainPrompt")
        .getChildFile("installed_plugins.json");
  }

  // ── AsyncUpdater: fires rescan listeners safely on Message Thread ──
  void handleAsyncUpdate() override {
    const juce::ScopedLock sl(listenerLock);
    for (auto& entry : rescanListeners)
      entry.callback(pendingRescanDiff);
  }

private:
  juce::AudioPluginFormatManager formatManager;
  juce::KnownPluginList knownPluginList;
  juce::OwnedArray<juce::PluginDescription> cachedPlugins;
  std::atomic<bool> scanComplete{false};
  std::atomic<bool> isRescan{false};

  juce::File cacheFile{
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getChildFile("ChainPrompt")
          .getChildFile("PluginCache.xml")};

  juce::File deadMansPedalFile{
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getChildFile("ChainPrompt")
          .getChildFile("scan_deadmans.txt")};

  juce::CriticalSection listenerLock;

  // Initial-scan listeners (one-shot, cleared after first scan)
  struct ListenerEntry { int id; std::function<void()> callback; };
  std::vector<ListenerEntry> pendingListeners;
  int nextListenerId = 1;

  // Rescan listeners (persistent, removable by ID)
  struct RescanListenerEntry { int id; std::function<void(const RescanDiff&)> callback; };
  std::vector<RescanListenerEntry> rescanListeners;

  // Pending diff stored by scan thread, consumed by handleAsyncUpdate
  RescanDiff pendingRescanDiff;

  // Rescan diff tracking
  std::set<int> preRescanUIDs;
  std::map<int, juce::String> preRescanNames;
  std::map<int, juce::String> preRescanVersions;

  // Track plugins that failed to scan
  juce::StringArray failedScanFiles;

  // Scan mode for manifest reporting
  juce::String currentScanMode{"full_vst3"};

  // -----------------------------------------------------------------------
  void exportInstalledManifest() {
    auto manifestFile = getManifestFile();
    manifestFile.getParentDirectory().createDirectory();

    auto* root = new juce::DynamicObject();
    root->setProperty("version", 2);
    root->setProperty("timestamp", juce::Time::currentTimeMillis());
    root->setProperty("scanMode", currentScanMode.isEmpty() ? "full_vst3" : currentScanMode);

    juce::Array<juce::var> pluginsArray;
    for (auto *desc : cachedPlugins) {
      auto* p = new juce::DynamicObject();
      p->setProperty("name", desc->name);
      p->setProperty("uid", desc->uniqueId);
      p->setProperty("category", desc->category);
      p->setProperty("manufacturer", desc->manufacturerName);
      p->setProperty("format", desc->pluginFormatName);
      p->setProperty("pluginFile", desc->fileOrIdentifier);
      p->setProperty("isInstrument", desc->isInstrument);
      p->setProperty("numInputChannels", desc->numInputChannels);
      p->setProperty("numOutputChannels", desc->numOutputChannels);
      p->setProperty("version", desc->version);
      pluginsArray.add(juce::var(p));
    }
    root->setProperty("plugins", pluginsArray);

    juce::Array<juce::var> failedArray;
    for (auto& f : failedScanFiles)
      failedArray.add(f);
    root->setProperty("failedFiles", failedArray);

    auto jsonStr = juce::JSON::toString(juce::var(root));
    manifestFile.replaceWithText(jsonStr);

    DBG("GlobalPluginCache: exported manifest v2 (" +
        juce::String(cachedPlugins.size()) + " plugins, " +
        juce::String(failedScanFiles.size()) + " failed) to " +
        manifestFile.getFullPathName());
  }

  // -----------------------------------------------------------------------
  RescanDiff computeRescanDiff() const {
    RescanDiff diff;
    diff.totalInstalled = cachedPlugins.size();
    diff.scanMode = preRescanUIDs.empty() ? "full_vst3" : "differential";

    std::set<int> postUIDs;
    std::map<int, juce::String> postNames;
    std::map<int, juce::String> postVersions;
    for (auto *desc : cachedPlugins) {
      postUIDs.insert(desc->uniqueId);
      postNames[desc->uniqueId] = desc->name;
      postVersions[desc->uniqueId] = desc->version;
    }

    for (int uid : postUIDs) {
      if (preRescanUIDs.find(uid) == preRescanUIDs.end()) {
        diff.added.add(postNames[uid]);
      } else {
        // Existed before — check for version update
        auto preV = preRescanVersions.find(uid);
        if (preV != preRescanVersions.end() && preV->second != postVersions[uid])
          diff.updated.add(postNames[uid]);
      }
    }

    for (int uid : preRescanUIDs) {
      if (postUIDs.find(uid) == postUIDs.end()) {
        auto it = preRescanNames.find(uid);
        diff.removed.add(it != preRescanNames.end() ? it->second : juce::String(uid));
      }
    }

    return diff;
  }

  // -----------------------------------------------------------------------
  class ScannerThread : public juce::Thread {
  public:
    explicit ScannerThread(GlobalPluginCache &c)
        : juce::Thread("GlobalPluginScanner"), cache(c) {}

    void run() override {
      auto* vst3Format = cache.formatManager.getFormat(0);
      if (vst3Format == nullptr) {
        DBG("GlobalPluginCache: ERROR — no VST3 format available");
        cache.scanComplete.store(true);
        return;
      }

      auto searchPaths = vst3Format->getDefaultLocationsToSearch();
      bool isDifferential = cache.knownPluginList.getNumTypes() > 0;

      DBG("GlobalPluginCache: " + juce::String(isDifferential ? "differential" : "full") +
          " VST3 scan (" + juce::String(cache.knownPluginList.getNumTypes()) + " known)...");
      for (int i = 0; i < searchPaths.getNumPaths(); ++i)
        DBG("  Search path: " + searchPaths[i].getFullPathName());

      cache.deadMansPedalFile.getParentDirectory().createDirectory();

      juce::PluginDirectoryScanner scanner(
          cache.knownPluginList,
          *vst3Format,
          searchPaths,
          true,
          cache.deadMansPedalFile
      );

      juce::String currentPlugin;
      int scannedCount = 0;

      while (scanner.scanNextFile(true, currentPlugin)) {
        if (threadShouldExit()) return;

        ++scannedCount;
        if (scannedCount % 20 == 0) {
          DBG("GlobalPluginCache: scanned " + juce::String(scannedCount) +
              " files... (current: " + currentPlugin + ")");
        }
      }

      cache.failedScanFiles = scanner.getFailedFiles();

      // Prune removed plugins (file no longer exists on disk)
      {
        int prunedCount = 0;
        auto allTypes = cache.knownPluginList.getTypes();  // getTypes() returns a copy
        for (auto& desc : allTypes) {
          if (!juce::File(desc.fileOrIdentifier).existsAsFile()) {
            cache.knownPluginList.removeType(desc);
            ++prunedCount;
            DBG("GlobalPluginCache: pruned uninstalled plugin: " + desc.name);
          }
        }
        if (prunedCount > 0)
          DBG("GlobalPluginCache: pruned " + juce::String(prunedCount) + " removed plugins");
      }

      // Rebuild cachedPlugins from the pruned knownPluginList
      for (auto &desc : cache.knownPluginList.getTypes())
        cache.cachedPlugins.add(new juce::PluginDescription(desc));

      DBG("GlobalPluginCache: scan complete — " +
          juce::String(cache.cachedPlugins.size()) + " plugins discovered" +
          (isDifferential ? " (differential)" : "") + ", " +
          juce::String(scannedCount) + " files checked, " +
          juce::String(cache.failedScanFiles.size()) + " failed");

      if (cache.failedScanFiles.size() > 0) {
        DBG("Failed plugins:");
        for (auto& f : cache.failedScanFiles)
          DBG("  " + f);
      }

      auto xml = cache.knownPluginList.createXml();
      if (xml != nullptr) {
        cache.cacheFile.getParentDirectory().createDirectory();
        xml->writeTo(cache.cacheFile);
        DBG("GlobalPluginCache: saved disk cache to " +
            cache.cacheFile.getFullPathName());
      }

      cache.currentScanMode = isDifferential ? "differential" : "full_vst3";
      cache.exportInstalledManifest();
      cache.scanComplete.store(true);

      bool wasRescan = cache.isRescan.exchange(false);

      if (wasRescan) {
        // Store diff for handleAsyncUpdate to consume on Message Thread
        cache.pendingRescanDiff = cache.computeRescanDiff();
        cache.triggerAsyncUpdate();
      }

      // Notify initial-scan listeners
      const juce::ScopedLock sl(cache.listenerLock);
      for (auto &entry : cache.pendingListeners)
        juce::MessageManager::callAsync(std::move(entry.callback));
      cache.pendingListeners.clear();
    }

  private:
    GlobalPluginCache &cache;
  };

  ScannerThread scannerThread{*this};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GlobalPluginCache)
};

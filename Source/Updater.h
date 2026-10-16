#pragma once
// In-plugin update check + one-click install. Shared verbatim between WXRST, WXRST XTRACT and Denny Key Finder
// (keep the three copies identical). Reads a public manifest, compares with this build's version, downloads the
// zip for this platform, and on macOS copies the new bundles into ~/Library/Audio/Plug-Ins; on Windows it unzips
// and launches install-windows.bat (shipped in the zip) which asks for admin and copies into Common Files\VST3.
//
//   manifest: https://raw.githubusercontent.com/kubankid/denny-plugins-releases/main/version.json
//   { "wxrst": { "version": "1.2.0", "notes": "...", "mac": "<zip url>", "win": "<zip url>" }, ... }
//
// Env overrides (tests / screenshots): DENNY_UPDATE_URL=<manifest url or file>, DENNY_NO_UPDATE=1,
// DENNY_UPDATE_FORCE=1 (ignore the 6 h throttle).
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

class Updater : private juce::Timer
{
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Installing, Installed, NeedsManual, Failed };

    // key = manifest entry ("wxrst"), version = this build ("1.2.0"), bundles = file names to replace on macOS
    Updater (juce::String key, juce::String version, juce::StringArray bundles, juce::String displayName)
        : key (std::move (key)), current (std::move (version)), bundleNames (std::move (bundles)), name (std::move (displayName)),
          shared (std::make_shared<Shared>())
    {
        const auto settings = cacheFile();
        if (auto v = juce::JSON::parse (settings); v.isObject())
        {
            dismissed = v.getProperty ("dismissed", "").toString();
            lastCheck = (juce::int64) v.getProperty ("lastCheck", 0.0);
        }
    }

    ~Updater() override { stopTimer(); download.reset(); }

    static juce::String defaultManifestURL() { return "https://raw.githubusercontent.com/kubankid/denny-plugins-releases/main/version.json"; }

    // Call from the message thread. Does nothing when offline or disabled; results show up in state().
    void checkNow (bool force = false)
    {
        if (juce::SystemStats::getEnvironmentVariable ("DENNY_NO_UPDATE", "") == "1") return;
        if (juce::SystemStats::getEnvironmentVariable ("DENNY_UPDATE_FORCE", "") == "1") force = true;
        const auto now = juce::Time::currentTimeMillis();
        if (! force && now - lastCheck < 6 * 60 * 60 * 1000) { if (! cachedLatest.isEmpty()) applyManifest (cachedLatest, cachedNotes, cachedURL); return; }
        auto st = shared;
        { const juce::SpinLock::ScopedLockType l (st->lock); if (st->state == State::Checking || st->state == State::Downloading || st->state == State::Installing) return; st->state = State::Checking; }
        lastCheck = now;
        saveCache();
        const juce::String url = juce::SystemStats::getEnvironmentVariable ("DENNY_UPDATE_URL", defaultManifestURL());
        const juce::String k = key, cur = current, dis = dismissed;
        juce::Thread::launch ([st, url, k, cur, dis]
        {
            juce::String text;
            juce::File local (url.startsWith ("/") ? url : url.startsWith ("file://") ? url.fromFirstOccurrenceOf ("file://", false, false) : juce::String());
            if (local.existsAsFile()) text = local.loadFileAsString();
            else
            {
                auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs (5000);
                if (auto in = juce::URL (url).createInputStream (opts)) text = in->readEntireStreamAsString();
            }
            juce::String latest, notes, zip; bool ok = false;
            if (auto v = juce::JSON::parse (text); v.isObject())
                if (auto entry = v.getProperty (k, juce::var()); entry.isObject())
                {
                    latest = entry.getProperty ("version", "").toString();
                    notes = entry.getProperty ("notes", "").toString();
                   #if JUCE_MAC
                    zip = entry.getProperty ("mac", "").toString();
                   #elif JUCE_WINDOWS
                    zip = entry.getProperty ("win", "").toString();
                   #endif
                    ok = latest.isNotEmpty();
                }
            const juce::SpinLock::ScopedLockType l (st->lock);
            if (! ok) { st->state = State::Failed; st->error = text.isEmpty() ? "offline" : "bad manifest"; return; }
            st->latest = latest; st->notes = notes; st->zipURL = zip;
            st->state = (isNewer (latest, cur) && zip.isNotEmpty()) ? State::Available : State::UpToDate;
        });
    }

    State state() const { const juce::SpinLock::ScopedLockType l (shared->lock); return shared->state; }
    juce::String latestVersion() const { const juce::SpinLock::ScopedLockType l (shared->lock); return shared->latest; }
    juce::String notes() const { const juce::SpinLock::ScopedLockType l (shared->lock); return shared->notes; }
    juce::String currentVersion() const { return current; }
    float progress() const { return progressValue.load(); }
    bool isDismissed() const { return dismissed.isNotEmpty() && dismissed == latestVersion(); }
    void dismiss() { dismissed = latestVersion(); saveCache(); }

    // Text for a banner, or empty when nothing should be shown
    juce::String bannerText() const
    {
        switch (state())
        {
            case State::Available:   return "UPDATE " + name + " v" + latestVersion() + " AVAILABLE  -  CLICK TO INSTALL" + (notes().isNotEmpty() ? "   (" + notes() + ")" : "");
            case State::Downloading: return "DOWNLOADING v" + latestVersion() + "  " + juce::String ((int) (progress() * 100)) + "%";
            case State::Installing:  return "INSTALLING...";
           #if JUCE_MAC
            case State::Installed:   return "INSTALLED v" + latestVersion() + "  -  restart FL Studio (or rescan plugins) to use it";
           #else
            case State::Installed:   return "DOWNLOADED v" + latestVersion() + "  -  close FL Studio and finish the installer window";
           #endif
            case State::NeedsManual: return "DOWNLOADED v" + latestVersion() + "  -  could not copy automatically; the folder is open, drag the files in";
            case State::Failed:      return failedVisible ? "UPDATE FAILED  -  " + shared->error : juce::String();
            default: return {};
        }
    }

    // The banner click: download the zip to ~/Downloads and install it
    void downloadAndInstall()
    {
        const auto s = state();
        if (s == State::Installed || s == State::NeedsManual) { revealDownload(); return; }
        if (s != State::Available) return;
        auto downloads = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
        downloads.createDirectory();
        zipFile = downloads.getChildFile (juce::File::createLegalFileName (name + " " + latestVersion() + ".zip"));
        partFile = zipFile.getSiblingFile (zipFile.getFileName() + ".part");
        partFile.deleteFile();
        juce::String url; { const juce::SpinLock::ScopedLockType l (shared->lock); url = shared->zipURL; }
        download = juce::URL (url).downloadToFile (partFile, juce::URL::DownloadTaskOptions());
        if (download == nullptr) { setState (State::Failed, "could not start download"); failedVisible = true; return; }
        progressValue.store (0.f);
        setState (State::Downloading);
        startTimer (200);
    }

    void revealDownload() { if (unzipDir.exists()) unzipDir.revealToUser(); else if (zipFile.existsAsFile()) zipFile.revealToUser(); }

    static bool isNewer (const juce::String& a, const juce::String& b)   // a > b ?
    {
        juce::StringArray pa, pb; pa.addTokens (a, ".", ""); pb.addTokens (b, ".", "");
        for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
        {
            const int x = i < pa.size() ? pa[i].retainCharacters ("0123456789").getIntValue() : 0;
            const int y = i < pb.size() ? pb[i].retainCharacters ("0123456789").getIntValue() : 0;
            if (x != y) return x > y;
        }
        return false;
    }

private:
    struct Shared { juce::SpinLock lock; State state = State::Idle; juce::String latest, notes, zipURL, error; };

    void setState (State s, juce::String err = {}) { const juce::SpinLock::ScopedLockType l (shared->lock); shared->state = s; if (err.isNotEmpty()) shared->error = err; }

    void applyManifest (const juce::String& latest, const juce::String& n, const juce::String& url)
    {
        const juce::SpinLock::ScopedLockType l (shared->lock);
        shared->latest = latest; shared->notes = n; shared->zipURL = url;
        shared->state = (isNewer (latest, current) && url.isNotEmpty()) ? State::Available : State::UpToDate;
    }

    void timerCallback() override
    {
        if (download == nullptr) { stopTimer(); return; }
        const auto total = download->getTotalLength(), got = download->getLengthDownloaded();
        if (total > 0) progressValue.store ((float) ((double) got / (double) total));
        if (! download->isFinished()) return;
        stopTimer();
        const bool bad = download->hadError() || partFile.getSize() < 100 * 1024;
        download.reset();
        if (bad) { partFile.deleteFile(); setState (State::Failed, "download failed - check your internet connection"); failedVisible = true; return; }
        zipFile.deleteFile();
        partFile.moveFileTo (zipFile);
        install();
    }

    void install()
    {
        setState (State::Installing);
        unzipDir = zipFile.getSiblingFile (zipFile.getFileNameWithoutExtension());
        unzipDir.deleteRecursively();
        unzipDir.createDirectory();
        juce::ZipFile zip (zipFile);
        if (zip.uncompressTo (unzipDir, true).failed()) { setState (State::Failed, "could not unzip"); failedVisible = true; return; }
       #if JUCE_MAC
        auto plugins = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins");
        int copied = 0, wanted = 0;
        for (auto& b : bundleNames)
        {
            auto src = findIn (unzipDir, b);
            if (! src.exists()) continue;
            ++wanted;
            auto dstDir = plugins.getChildFile (b.endsWith (".component") ? "Components" : "VST3");
            dstDir.createDirectory();
            auto dst = dstDir.getChildFile (b);
            dst.deleteRecursively();
            if (src.copyDirectoryTo (dst)) ++copied;
        }
        if (wanted > 0 && copied == wanted) setState (State::Installed);
        else { unzipDir.revealToUser(); setState (State::NeedsManual); }
       #elif JUCE_WINDOWS
        auto bat = findIn (unzipDir, "install-windows.bat");
        if (bat.existsAsFile() && bat.startAsProcess()) setState (State::Installed);
        else { unzipDir.revealToUser(); setState (State::NeedsManual); }
       #else
        unzipDir.revealToUser(); setState (State::NeedsManual);
       #endif
        updateCacheAfterInstall();
    }

    static juce::File findIn (const juce::File& dir, const juce::String& fileName)
    {
        auto direct = dir.getChildFile (fileName);
        if (direct.exists()) return direct;
        for (const auto& e : juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFilesAndDirectories))
            if (e.getFile().getFileName() == fileName) return e.getFile();
        return {};
    }

    juce::File cacheFile() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("DennyPlugins");
        dir.createDirectory();
        return dir.getChildFile ("updates-" + key + ".json");
    }
    void saveCache()
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("lastCheck", (double) lastCheck);
        o->setProperty ("dismissed", dismissed);
        o->setProperty ("latest", cachedLatest = latestVersion());
        o->setProperty ("notes", cachedNotes = notes());
        { const juce::SpinLock::ScopedLockType l (shared->lock); o->setProperty ("url", cachedURL = shared->zipURL); }
        cacheFile().replaceWithText (juce::JSON::toString (juce::var (o)));
    }
    void updateCacheAfterInstall() { dismissed = latestVersion(); saveCache(); }

    juce::String key, current; juce::StringArray bundleNames; juce::String name;
    std::shared_ptr<Shared> shared;
    juce::String dismissed, cachedLatest, cachedNotes, cachedURL;
    juce::int64 lastCheck = 0;
    bool failedVisible = false;    // a failed silent check stays silent; a failed download is shown
    std::unique_ptr<juce::URL::DownloadTask> download;
    juce::File zipFile, partFile, unzipDir;
    std::atomic<float> progressValue { 0.f };
};

// Thin strip that shows the updater's message; click = install, the x at the right = dismiss this version.
class UpdateBanner : public juce::Component, private juce::Timer
{
public:
    explicit UpdateBanner (Updater& u) : updater (u) { setVisible (false); startTimerHz (4); }
    void setColours (juce::Colour background, juce::Colour accent, juce::Colour textColour) { bg = background; acc = accent; txt = textColour; repaint(); }
    std::function<void (bool)> onVisibilityChanged;   // parent may re-layout when the strip appears / goes away

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (bg); g.fillRoundedRectangle (r, 5.f);
        g.setColour (acc.withAlpha (0.9f)); g.drawRoundedRectangle (r.reduced (0.5f), 5.f, 1.f);
        g.setColour (txt);
        g.setFont (juce::Font (juce::FontOptions (11.f, juce::Font::bold)));
        g.drawText (text, getLocalBounds().reduced (10, 0).withTrimmedRight (20), juce::Justification::centredLeft, true);
        if (updater.state() == Updater::State::Available)
        {
            g.setColour (txt.withAlpha (0.7f));
            g.drawText ("x", closeArea(), juce::Justification::centred);
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (closeArea().contains (e.getPosition()) && updater.state() == Updater::State::Available) { updater.dismiss(); refresh(); return; }
        updater.downloadAndInstall();
        refresh();
    }
    void mouseEnter (const juce::MouseEvent&) override { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

private:
    juce::Rectangle<int> closeArea() const { return getLocalBounds().removeFromRight (20); }
    void timerCallback() override { refresh(); }
    void refresh()
    {
        auto t = updater.bannerText();
        const bool show = t.isNotEmpty() && ! (updater.state() == Updater::State::Available && updater.isDismissed());
        if (t != text) { text = t; repaint(); }
        if (show != isVisible()) { setVisible (show); if (onVisibilityChanged) onVisibilityChanged (show); }
        if (show && ! autoClicked && updater.state() == Updater::State::Available
            && juce::SystemStats::getEnvironmentVariable ("DENNY_UPDATE_AUTOINSTALL", "") == "1") { autoClicked = true; updater.downloadAndInstall(); }
    }
    Updater& updater;
    juce::String text;
    bool autoClicked = false;
    juce::Colour bg { 0xff0e1a2a }, acc { 0xff6fe3ff }, txt { juce::Colours::white };
};

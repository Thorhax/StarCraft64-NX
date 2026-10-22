#include "voices.hpp"
#include "log.hpp"
#include <json/json.hpp>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace sc64::voices {
namespace {
// RDRAM locations found with the briefing probe (StarCraft 64 USA, menu overlay).
constexpr std::uint32_t script_pointer_address = 0x800dd028; // -> "<OBJECTIVE>...<PORTn><TEXT>..." script
constexpr std::uint32_t page_address = 0x800ddb58;           // current <TEXT> page, 0-based
constexpr float voice_gain = 0.9f, music_duck = 0.45f;
constexpr double mission_threshold = 0.25, page_threshold = 0.12;

using Words = std::vector<std::uint32_t>; // sorted, unique word hashes

Words tokenize(std::string_view text) {
    Words words;
    std::uint32_t hash = 2166136261u;
    unsigned length = 0;
    auto flush = [&] {
        if (length >= 3) words.push_back(hash);
        hash = 2166136261u;
        length = 0;
    };
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { hash = (hash ^ c) * 16777619u; ++length; }
        else flush();
    }
    flush();
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return words;
}

double overlap(const Words& a, const Words& b) {
    if (a.empty() || b.empty()) return 0;
    std::size_t i = 0, j = 0, common = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) { ++common; ++i; ++j; }
        else if (a[i] < b[j]) ++i;
        else ++j;
    }
    return double(common) / double(std::min(a.size(), b.size()));
}

struct Mission {
    std::string key;
    std::vector<Words> texts;       // English transmission texts (matching)
    std::vector<std::string> files; // voice files in the chosen language
};
std::vector<Mission> missions;
std::string language_root;
double sample_step = 0.5; // source samples per 48 kHz output sample
bool enabled = false;  // briefing matching (the enUS index is installed)
bool playback = true;  // voice playback (false with novoices)
std::mutex briefing_mutex;
Briefing briefing;

// ---- In-mission transmissions (ingame.json, tools/voices/extract_ingame_voices.py)
constexpr std::uint32_t message_id_address = 0x800b03d8;   // advisor/transmission box, 0xFFFF when closed
constexpr std::uint32_t message_text_address = 0x800b03e0; // -> the text it shows
constexpr double line_threshold = 0.6;
struct Line { Words words; std::string file; };
std::map<std::string, std::vector<Line>> ingame; // by PC mission key
std::string ingame_root;
std::uint32_t shown_text = 0;

// ---- Briefing state (guest thread only)
std::uint32_t script_ptr = 0;
int last_page = -1, mission = -1, playing = -1;
std::vector<int> page_transmission;

// ---- Playback: a loader thread reads files; the audio thread mixes.
// Heap-allocated and never destroyed: the detached loader may still be waiting
// on these at process exit, and destroying them under it hangs the exit.
struct Shared {
    std::mutex load_mutex;
    std::condition_variable load_wake;
    std::string load_path;
    std::uint64_t generation = 0; // bumped by every play/stop; stale loads are dropped
    std::mutex audio_mutex;
    std::shared_ptr<const std::vector<std::int16_t>> voice;
    double position = 0;
    float duck = 1.0f;
};
Shared& shared = *new Shared;
std::mutex& load_mutex = shared.load_mutex;
std::condition_variable& load_wake = shared.load_wake;
std::string& load_path = shared.load_path;
std::uint64_t& generation = shared.generation;
std::mutex& audio_mutex = shared.audio_mutex;
std::shared_ptr<const std::vector<std::int16_t>>& voice = shared.voice;
double& position = shared.position;
float& duck = shared.duck;

void loader() {
    std::uint64_t seen = 0;
    for (;;) {
        std::string path;
        std::uint64_t request;
        {
            std::unique_lock lock(load_mutex);
            load_wake.wait(lock, [&] { return generation != seen; });
            seen = request = generation;
            path = load_path;
        }
        if (path.empty()) continue;
        auto samples = std::make_shared<std::vector<std::int16_t>>();
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file) {
            samples->resize(std::size_t(file.tellg()) / 2);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(samples->data()), std::streamsize(samples->size() * 2));
        } else {
            sc64::shell::log::warning("voices: cannot read %s", path.c_str());
            continue;
        }
        std::lock_guard lock(load_mutex);
        if (request != generation) continue; // superseded while loading
        std::lock_guard audio(audio_mutex);
        voice = std::move(samples);
        position = 0;
    }
}

void play(const std::string& path) {
    {
        // Same lock order as the loader (load, then audio): the previous voice
        // stops at once, and a load finishing now sees the new generation.
        std::lock_guard lock(load_mutex);
        {
            std::lock_guard audio(audio_mutex);
            voice.reset();
        }
        load_path = path;
        ++generation;
    }
    load_wake.notify_one();
}

void stop() { play(""); }

std::uint32_t word(const std::uint8_t* ram, std::uint32_t a) {
    const unsigned p = a & 0x7fffff;
    return (std::uint32_t(ram[p ^ 3]) << 24) | (std::uint32_t(ram[(p + 1) ^ 3]) << 16)
         | (std::uint32_t(ram[(p + 2) ^ 3]) << 8) | ram[(p + 3) ^ 3];
}

// N64 script: <OBJECTIVE>..., <PORTn> (portrait), <TEXT> pages "Speaker\r\n\r\nBody".
std::vector<Words> read_pages(const std::uint8_t* ram, std::uint32_t address) {
    std::string script;
    for (std::uint32_t i = 0; i < 32768; ++i) {
        const std::uint32_t a = (address + i) & 0x7fffff;
        const char c = char(ram[a ^ 3]);
        if (c == 0) break;
        script.push_back(c);
    }
    std::vector<Words> pages;
    for (std::size_t at = script.find("<TEXT>"); at != std::string::npos; at = script.find("<TEXT>", at + 6)) {
        std::size_t begin = at + 6;
        const std::size_t end = std::min(script.find('<', begin), script.size());
        std::string_view page(script.data() + begin, end - begin);
        // Skip the speaker line: PC transmission texts carry only the body.
        const auto first = page.find_first_not_of("\r\n");
        if (first != std::string_view::npos) {
            const auto line_end = page.find('\n', first);
            page = line_end == std::string_view::npos ? std::string_view{} : page.substr(line_end + 1);
        }
        pages.push_back(tokenize(page));
    }
    return pages;
}

// Ordered alignment: each page maps to a transmission, never going backwards.
double align(const std::vector<Words>& pages, const Mission& m, std::vector<int>& mapping) {
    const std::size_t n = pages.size(), k = m.texts.size();
    if (!n || !k) return 0;
    std::vector<double> score(n * k), best(n * k);
    std::vector<int> from(n * k, -1);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < k; ++j) score[i * k + j] = overlap(pages[i], m.texts[j]);
    for (std::size_t j = 0; j < k; ++j) best[j] = score[j];
    for (std::size_t i = 1; i < n; ++i) {
        double prefix = -1;
        int arg = 0;
        for (std::size_t j = 0; j < k; ++j) {
            const double previous = best[(i - 1) * k + j];
            if (previous > prefix) { prefix = previous; arg = int(j); }
            best[i * k + j] = score[i * k + j] + prefix;
            from[i * k + j] = arg;
        }
    }
    int j = int(std::max_element(best.begin() + (n - 1) * k, best.end()) - (best.begin() + (n - 1) * k));
    const double total = best[(n - 1) * k + j];
    mapping.assign(n, -1);
    for (std::size_t i = n; i-- > 0;) {
        mapping[i] = score[i * k + j] >= page_threshold ? j : -1;
        if (i) j = from[i * k + j];
    }
    return total / double(n);
}

void match(const std::uint8_t* ram) {
    const auto pages = read_pages(ram, script_ptr);
    mission = -1;
    page_transmission.clear();
    double best = 0;
    std::vector<int> mapping;
    for (std::size_t m = 0; m < missions.size(); ++m) {
        const double score = align(pages, missions[m], mapping);
        if (score > best) { best = score; mission = int(m); page_transmission = mapping; }
    }
    if (mission < 0 || best < mission_threshold) {
        sc64::shell::log::info("voices: briefing with %zu pages not matched (best %.2f)", pages.size(), best);
        mission = -1;
        return;
    }
    {
        std::lock_guard lock(briefing_mutex);
        briefing.mission = missions[mission].key;
        ++briefing.serial;
    }
    std::string pages_text;
    for (int t : page_transmission) pages_text += " " + std::to_string(t);
    sc64::shell::log::info("voices: briefing matched %s (score %.2f), pages ->%s",
                           missions[mission].key.c_str(), best, pages_text.c_str());
}

std::vector<const nlohmann::json*> transmissions(const nlohmann::json& events) {
    std::vector<const nlohmann::json*> out;
    for (const auto& e : events)
        if (e.value("type", "") == "transmission") out.push_back(&e);
    return out;
}
bool campaign_key(const std::string& key) {
    const auto slash = key.find('/');
    if (slash == std::string::npos || key.find("tutorial") != std::string::npos) return false;
    const std::string race = key.substr(0, slash);
    for (const char* r : {"Terran", "Zerg", "Protoss", "EXPTerran", "EXPZerg", "EXPProtoss"})
        if (race == r) return true;
    return false;
}
}

void initialize(const std::string& root) {
    namespace fs = std::filesystem;
    std::error_code ignored;
    if (fs::exists(fs::path(root).parent_path() / "novoices", ignored)
        || fs::exists(fs::path(root).parent_path() / "novoices.txt", ignored)) {
        sc64::shell::log::info("voices: playback disabled (novoices); briefings are still matched");
        playback = false;
    }
    std::string language = "enUS";
    if (std::ifstream file(fs::path(root) / "language"); file) {
        std::string value;
        file >> value;
        if (!value.empty()) language = value;
    }
    try {
        std::ifstream index_file(fs::path(root) / "enUS" / "voices.json");
        if (!index_file) { sc64::shell::log::info("voices: no %s/enUS/voices.json, briefings stay silent", root.c_str()); return; }
        const auto index = nlohmann::json::parse(index_file);
        nlohmann::json chosen = index;
        if (language != "enUS") {
            std::ifstream language_file(fs::path(root) / language / "voices.json");
            if (language_file) chosen = nlohmann::json::parse(language_file);
            else { sc64::shell::log::warning("voices: no %s voices, using enUS", language.c_str()); language = "enUS"; }
        }
        language_root = (fs::path(root) / language).string();
        sample_step = double(chosen.value("sample_rate", 48000)) / 48000.0;
        for (const auto& [key, events] : index.at("missions").items()) {
            if (!campaign_key(key)) continue;
            Mission m;
            m.key = key;
            for (const auto* e : transmissions(events)) m.texts.push_back(tokenize(e->value("text", "")));
            const auto& local = chosen.at("missions");
            std::vector<const nlohmann::json*> files;
            if (local.contains(key)) files = transmissions(local.at(key));
            for (std::size_t i = 0; i < m.texts.size(); ++i)
                m.files.push_back(i < files.size() ? files[i]->value("file", "") : "");
            missions.push_back(std::move(m));
        }
    } catch (const std::exception& error) {
        sc64::shell::log::warning("voices: %s", error.what());
        missions.clear();
        return;
    }
    // In-mission voices exist in English only (the N64 text is English).
    try {
        if (std::ifstream lines_file(fs::path(root) / "enUS" / "ingame.json"); lines_file) {
            const auto lines = nlohmann::json::parse(lines_file);
            std::size_t count = 0;
            for (const auto& [key, entries] : lines.at("missions").items())
                for (const auto& entry : entries) {
                    ingame[key].push_back({tokenize(entry.value("text", "")), entry.value("file", "")});
                    ++count;
                }
            ingame_root = (fs::path(root) / "enUS").string();
            sc64::shell::log::info("voices: %zu in-mission transmissions", count);
        }
    } catch (const std::exception& error) {
        sc64::shell::log::warning("voices: ingame.json: %s", error.what());
        ingame.clear();
    }
    enabled = !missions.empty();
    if (enabled && playback) {
#if defined(__SWITCH__)
        // libnx thread: std::thread construction terminated the process here.
        static Thread loader_thread;
        if (R_FAILED(threadCreate(&loader_thread, [](void*) { loader(); }, nullptr, nullptr, 0x40000, 0x2C, -2))
            || R_FAILED(threadStart(&loader_thread))) {
            sc64::shell::log::warning("voices: loader thread failed, briefings stay silent");
            playback = false;
            return;
        }
#else
        std::thread(loader).detach();
#endif
        sc64::shell::log::info("voices: %zu campaign briefings, language %s", missions.size(), language.c_str());
    }
}

void observe(const std::uint8_t* ram, bool active) {
    if (!enabled) return;
    if (!active) {
        if (script_ptr && playback) stop();
        script_ptr = 0; mission = -1; last_page = -1; playing = -1;
        return;
    }
    const std::uint32_t pointer = word(ram, script_pointer_address);
    if (pointer < 0x80000000 || pointer >= 0x80800000) return;
    if (pointer != script_ptr) {
        script_ptr = pointer;
        last_page = -1; playing = -1;
        match(ram);
    }
    const int page = int(word(ram, page_address));
    if (page == last_page) return;
    last_page = page;
    const int transmission = mission >= 0 && page >= 0 && page < int(page_transmission.size())
                             ? page_transmission[page] : -1;
    if (transmission == playing || !playback) return; // same speaker continues on the next page
    playing = transmission;
    const std::string& file = transmission >= 0 ? missions[mission].files[transmission] : std::string();
    if (file.empty()) stop();
    else play(language_root + "/" + file);
}

void observe_messages(const std::uint8_t* ram) {
    if (ingame.empty() || !playback) return;
    if ((word(ram, message_id_address) & 0xffff) == 0xffff) { shown_text = 0; return; } // the voice plays on
    const std::uint32_t text = word(ram, message_text_address);
    if (text == shown_text || text < 0x80000000 || text >= 0x80800000) return;
    shown_text = text;
    std::string message;
    for (std::uint32_t i = 0; i < 1024; ++i) {
        const char c = char(ram[((text + i) & 0x7fffff) ^ 3]);
        if (c == 0) break;
        message.push_back(c);
    }
    const Words words = tokenize(message);
    const std::string mission = last_briefing().mission;
    // The briefed mission first; a mission loaded from a save searches them all.
    const Line* best = nullptr;
    double best_score = 0;
    auto search = [&](const std::vector<Line>& lines) {
        for (const auto& line : lines)
            if (const double score = overlap(words, line.words); score > best_score) { best_score = score; best = &line; }
    };
    if (const auto it = ingame.find(mission); it != ingame.end()) search(it->second);
    if (best_score < line_threshold)
        for (const auto& [key, lines] : ingame) search(lines);
    if (!best || best_score < line_threshold) return; // advisor hints and objectives have no voice
    sc64::shell::log::info("voices: transmission %s (score %.2f)", best->file.c_str(), best_score);
    play(ingame_root + "/" + best->file);
}

Briefing last_briefing() {
    std::lock_guard lock(briefing_mutex);
    return briefing;
}

void mix(float* stereo, unsigned frames) {
    if (!enabled || !playback) return;
    std::lock_guard lock(audio_mutex);
    const auto current = voice;
    const float target = current ? music_duck : 1.0f;
    for (unsigned f = 0; f < frames; ++f) {
        duck += (target - duck) * 0.0005f; // ~50 ms glide
        float sample = 0;
        if (current) {
            const std::size_t i = std::size_t(position);
            if (i + 1 < current->size()) {
                const double t = position - double(i);
                sample = float((*current)[i] * (1 - t) + (*current)[i + 1] * t) / 32768.0f * voice_gain;
                position += sample_step;
            } else voice.reset();
        }
        for (int c = 0; c < 2; ++c) {
            float& out = stereo[f * 2 + c];
            out = std::clamp(out * duck + sample, -1.0f, 1.0f);
        }
    }
}
}

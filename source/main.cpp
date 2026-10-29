// Splat3GearView NX — Ultrahand/Tesla overlay
// Reads the 3 worn Splatoon 3 gear records directly from game memory while
// the game is in the foreground. Shares the same core as the NRO build.
#define TESLA_INIT_IMPL
// stubs for the disabled download_funcs (no curl/mbedtls portlibs here)
#include <cstddef>
namespace ult {
    size_t DOWNLOAD_READ_BUFFER = 262144;
    size_t DOWNLOAD_WRITE_BUFFER = 262144;
    size_t UNZIP_READ_BUFFER = 262144;
    size_t UNZIP_WRITE_BUFFER = 262144;
}
#include <tesla.hpp>
#include <switch.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdarg>

extern "C" {
#include "s3gv.h"
#include "generated/gear_tables.h"
}

// globals gear.c expects (owned here in the overlay)
uint16_t g_loadout[3] = {0, 0, 0};
uint64_t g_gear_found_addr = 0;
GearInfo g_worn_info[3];
static bool g_attached = false;
static uint64_t g_gacha_addr = 0;
static uint64_t g_worn_addr[3] = {0, 0, 0};

static char g_status[256] = "Open the game, then open this overlay";

void app_log(const char* fmt, ...) {
    FILE* f = fopen("sdmc:/switch/Splat3GearViewNX/log.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

// Full worn-read pipeline (port of the NRO's read_loadout, UI-free)
static void read_worn() {
    app_log("---- ovl loadout start ----");
    if (mem_attach() != 0) {
        snprintf(g_status, sizeof(g_status), "ERROR: could not attach — game must be running");
        return;
    }
    g_attached = true;

    // resolve gacha anchor: fixed -> heap_base probe -> full scan
    uint64_t gacha = 0;
    GachaSeed seeds[8];
    int nseeds = gacha_read(GACHA_BLOCK, seeds, 8);
    if (nseeds > 0) gacha = GACHA_BLOCK;
    if (!gacha) {
        nseeds = gacha_read(GACHA_BLOCK + DLC_OFFS, seeds, 8);
        if (nseeds > 0) gacha = GACHA_BLOCK + DLC_OFFS;
    }
    if (!gacha) {
        uint64_t hb = mem_heap_base();
        if (hb) {
            static const uint64_t inheap[2] = { 0xBA416F70ULL, 0xBA416EF0ULL };
            for (int i = 0; i < 2 && !gacha; i++)
                for (int64_t d = -0x200; d <= 0x200 && !gacha; d += 0x80) {
                    GachaSeed s2[8];
                    if (gacha_read(hb + inheap[i] + d, s2, 8) > 0)
                        gacha = hb + inheap[i] + d;
                }
            if (gacha) app_log("gacha via heap_base+%llX", (unsigned long long)(gacha - hb));
        }
    }
    if (!gacha) {
        int64_t found = mem_locate_gacha();
        if (found < 0) {
            snprintf(g_status, sizeof(g_status), "ERROR: gacha block not found");
            mem_detach();
            g_attached = false;
            return;
        }
        gacha = (uint64_t)found;
    }
    g_gacha_addr = gacha;
    nseeds = gacha_read(gacha, seeds, 8);
    app_log("gacha anchor @%llX (seeds %d)", (unsigned long long)gacha, nseeds);

    // triplet at region+0x4F0
    uint64_t raddr = 0, rsize = 0;
    uint32_t rt = 0, rp = 0;
    mem_query(gacha, &raddr, &rsize, &rt, &rp);
    app_log("gacha region %llX sz %llX", (unsigned long long)raddr, (unsigned long long)rsize);
    uint8_t buf[0x40000];
    uint64_t found = 0;
    if (raddr && rsize) {
        uint64_t pos = 0;
        while (pos < rsize && !found) {
            size_t n = rsize - pos;
            if (n > sizeof(buf)) n = sizeof(buf);
            if (mem_read(raddr + pos, buf, n) != 0) { pos += n; continue; }
            for (size_t o = 0; o + 24 <= n; o += 4) {
                uint32_t a = buf[o] | (buf[o+1]<<8) | (buf[o+2]<<16) | (buf[o+3]<<24);
                uint32_t b = buf[o+8] | (buf[o+9]<<8) | (buf[o+10]<<16) | (buf[o+11]<<24);
                uint32_t c = buf[o+16] | (buf[o+17]<<8) | (buf[o+18]<<16) | (buf[o+19]<<24);
                if (!a || !b || !c || a > 0xFFFF || b > 0xFFFF || c > 0xFFFF) continue;
                uint32_t f1 = buf[o+4] | (buf[o+5]<<8) | (buf[o+6]<<16) | (buf[o+7]<<24);
                uint32_t f2 = buf[o+12] | (buf[o+13]<<8) | (buf[o+14]<<16) | (buf[o+15]<<24);
                uint32_t f3 = buf[o+20] | (buf[o+21]<<8) | (buf[o+22]<<16) | (buf[o+23]<<24);
                if (f1 > 1 || f2 > 1 || f3 > 1) continue;
                if (!gear_data_find(0, a) || !gear_data_find(1, b) || !gear_data_find(2, c)) continue;
                found = raddr + pos + o;
                g_loadout[0] = a; g_loadout[1] = b; g_loadout[2] = c;
                app_log("loadout @%llX: %u %u %u", (unsigned long long)found, a, b, c);
                break;
            }
            pos += n;
            if (n == sizeof(buf) && pos < rsize) pos -= 24;
        }
    }
    if (!found) {
        snprintf(g_status, sizeof(g_status), "Worn gear not found");
        mem_detach();
        g_attached = false;
        return;
    }

    // fast window scan around the stable gear-list area
    for (int t = 0; t < 3; t++) memset(&g_worn_info[t], 0, sizeof(GearInfo));
    uint64_t rbase = found - 0x4F0;
    uint64_t win_addr = rbase + 0x2B0000;
    uint32_t win_len = 0x46000;
    static uint8_t win[0x46000];
    if (mem_read(win_addr, win, win_len) == 0) {
        for (int t = 0; t < 3; t++) {
            for (uint32_t o = 0; o + GEAR_SIZE <= win_len; o += 4) {
                uint16_t gid = win[o] | (win[o + 1] << 8);
                if (gid != g_loadout[t] || !gear_validate(win + o)) continue;
                if (win[o + 0x54] < 1 || win[o + 0x68] > 3) continue;
                gear_parse(&g_worn_info[t], win + o, t);
                g_worn_addr[t] = win_addr + o;
                break;
            }
        }
    }
    // region-bounded dup-group scan for whatever is missing
    int missing = 0;
    for (int t = 0; t < 3; t++) if (g_loadout[t] && g_worn_info[t].id == 0) missing++;
    static const int order[3] = {1, 0, 2};
    if (missing && raddr && rsize) {
        g_scan_lo = raddr; g_scan_hi = raddr + rsize;
        for (int oi = 0; oi < 3; oi++) {
            int t = order[oi];
            if (!(g_loadout[t] && g_worn_info[t].id == 0)) continue;
            if (gear_find_struct(t, g_loadout[t], &g_worn_info[t]))
                g_worn_addr[t] = g_gear_found_addr;
        }
        g_scan_lo = 0; g_scan_hi = 0;
    }
    // stride fill from any resolved neighbor
    for (int t = 0; t < 3; t++) {
        if (!(g_loadout[t] && g_worn_info[t].id == 0)) continue;
        for (int n = 0; n < 3; n++) {
            if (!g_worn_addr[n] || n == t) continue;
            long long delta = ((long long)t - (long long)n) * 0x2C020LL;
            uint64_t cand = g_worn_addr[n] + (uint64_t)delta;
            uint8_t rec[GEAR_SIZE];
            if (mem_read(cand, rec, GEAR_SIZE) != 0) continue;
            uint16_t gid = rec[0] | (rec[1] << 8);
            if (gid != g_loadout[t] || !gear_validate(rec)) continue;
            gear_parse(&g_worn_info[t], rec, t);
            g_worn_addr[t] = cand;
        }
    }
    mem_detach();
    g_attached = false;

    int ok = 0;
    for (int t = 0; t < 3; t++) if (g_worn_info[t].id) ok++;
    snprintf(g_status, sizeof(g_status), ok == 3 ? "Read OK" : "Read: %d/3 items", ok);
}

// leanny seed-checker JSON export (same as NRO)
static bool dump_json(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    const char* names[3] = { "HaveGearHeadMap", "HaveGearClothesMap", "HaveGearShoesMap" };
    fprintf(f, "{\n");
    for (int t = 0; t < 3; t++) {
        fprintf(f, "%s\"%s\": {", t ? ",\n" : "", names[t]);
        bool first = true;
        const GearInfo* gi = &g_worn_info[t];
        if (gi->id) {
            const uint8_t* d = gi->raw;
            uint8_t p = d[0x54];
            int main_skill = (p - 1 >= 14) ? (p - 14) + 99 : p - 1;
            fprintf(f, "\n    \"%u\": { \"MainSkill\": %d, \"RandomContext\": %u, "
                       "\"ExDrinksArray\": [], \"ExSkillArray\": [%d, %d, %d] }",
                    gi->id, main_skill, gi->seed, d[0x6C] - 1, d[0x70] - 1, d[0x74] - 1);
            first = false;
        }
        (void)first;
        fprintf(f, "\n  }");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    return true;
}

static std::string gear_line(int t) {
    const GearDataEntry* e = gear_data_find(t, g_loadout[t]);
    const GearInfo* gi = &g_worn_info[t];
    char buf[256];
    if (!e) {
        snprintf(buf, sizeof(buf), "%s: -none-",
                 t == 0 ? "Head" : t == 1 ? "Clothes" : "Shoes");
        return buf;
    }
    if (!gi->id) {
        snprintf(buf, sizeof(buf), "%s: %s", t == 0 ? "Head" : t == 1 ? "Clothes" : "Shoes", e->name);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%s\n%s (%s)\nMain: %s\nSubs: %s / %s / %s\nSeed %08X  Rarity %u",
             t == 0 ? "Head" : t == 1 ? "Clothes" : "Shoes", e->name, e->brand,
             ability_name(gi->primary),
             ability_name(gi->subs[0]), ability_name(gi->subs[1]), ability_name(gi->subs[2]),
             gi->seed, gi->rarity);
    return buf;
}

// Results gui
class GuiResults : public tsl::Gui {
public:
    GuiResults() {}
    virtual tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("Splat3GearView NX", g_status);
        auto list = new tsl::elm::List();
        for (int t = 0; t < 3; t++)
            list->addItem(new tsl::elm::ListItem(gear_line(t)));
        auto* exportItem = new tsl::elm::ListItem("Export JSON (leanny seed checker)", "...", true);
        exportItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                if (dump_json("sdmc:/switch/Splat3GearViewNX/dump.json"))
                    snprintf(g_status, sizeof(g_status), "Exported -> leanny seed checker");
                else
                    snprintf(g_status, sizeof(g_status), "Export failed");
                tsl::changeTo<GuiResults>();
                return true;
            }
            return false;
        });
        list->addItem(exportItem);
        frame->setContent(list);
        return frame;
    }
};

class GuiMain : public tsl::Gui {
public:
    GuiMain() {}
    virtual tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("Splat3GearView NX", g_status);
        auto list = new tsl::elm::List();

        auto* readItem = new tsl::elm::ListItem("Read worn gear", "...", true);
        readItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                read_worn();
                tsl::changeTo<GuiResults>();
                return true;
            }
            return false;
        });
        list->addItem(readItem);
        list->addItem(new tsl::elm::ListItem(g_status));
        frame->setContent(list);
        return frame;
    }
};

class Splat3Overlay : public tsl::Overlay {
public:
    virtual void initServices() override {}
    virtual void exitServices() override {}
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiMain>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<Splat3Overlay>(argc, argv);
}

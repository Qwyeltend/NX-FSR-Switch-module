// NX-FSR overlay (Tesla / Ultrahand): per-game settings for the effect built into SaltyNX Core.
// Settings change live through a block in SaltyNX's shared memory and are saved to
// sdmc:/config/fsrSwitch/<TITLEID>.ini, which Core reads at the next game start.
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <cstdio>
#include <sys/stat.h>
#include "saltynx_ipc.hpp"
#include "fsr_shared.h"

namespace {
    // ---------------------------------------------------------------- texts (English / Russian)
    enum Text {
        T_EFFECT, T_ON, T_OFF, T_SHARPNESS, T_ANTIALIAS, T_COMPARE, T_FLIP, T_FLIP_AUTO, T_FLIP_OFF, T_FLIP_ON,
        T_STATUS, T_STATE, T_FRAME, T_GPU, T_ORIENTATION,
        T_ST_WAITING, T_ST_RUNNING, T_ST_FAILED, T_ST_NO_MEMORY, T_ST_OFF,
        T_NO_GAME, T_NO_SALTYNX, T_NO_BLOCK, T_HINT_FLIP, T_RESTART, T_SAVED_NOTE,
        T_ORIENT_UNKNOWN, T_ORIENT_UPPER, T_ORIENT_LOWER, T_COUNT
    };
    const char* const kEnglish[T_COUNT] = {
        "Effect", "On", "Off", "Sharpness", "Anti-aliasing", "Compare (left half)", "Picture flip", "Auto", "Off", "On",
        "Status", "State", "Frame", "GPU time", "Frame origin",
        "Waiting for frame", "Running", "Failed (see log)", "Restart game to enable", "Off",
        "Start a game first.", "SaltyNX is not running.", "NX-FSR is not active in this game.",
        "Use if the picture is upside down", "Restart the game to apply", "Saved for this game",
        "unknown", "top", "bottom",
    };
    const char* const kRussian[T_COUNT] = {
        "Эффект", "Вкл", "Выкл", "Резкость", "Сглаживание", "Сравнение (левая половина)", "Переворот картинки", "Авто", "Нет", "Да",
        "Состояние", "Статус", "Кадр", "Время GPU", "Начало кадра",
        "Ждёт кадр", "Работает", "Ошибка (см. лог)", "Перезапусти игру", "Выключен",
        "Сначала запусти игру.", "SaltyNX не запущен.", "NX-FSR не активен в этой игре.",
        "Если картинка вверх ногами", "Применится после перезапуска игры", "Сохранено для этой игры",
        "неизвестно", "сверху", "снизу",
    };
    bool g_russian = false;
    const char* T(Text t) { return g_russian ? kRussian[t] : kEnglish[t]; }

    // ---------------------------------------------------------------- shared memory
    SharedMemory g_shmem{};
    bool g_shmemMapped = false;
    bool g_saltyRunning = false;
    bool g_gameRunning = false;
    u64 g_titleId = 0;
    volatile FsrSharedBlock* g_block = nullptr;

    void FindBlock() {
        if (!g_shmemMapped || !g_titleId) return;
        const uintptr_t base = (uintptr_t)shmemGetAddr(&g_shmem);
        for (uintptr_t off = 0; off + sizeof(FsrSharedBlock) <= 0x1000; off += 4) {
            volatile FsrSharedBlock* b = (volatile FsrSharedBlock*)(base + off);
            if (b->magic == FSR_SHARED_MAGIC && b->version == FSR_SHARED_VERSION &&
                b->titleIdLow == (u32)g_titleId && b->titleIdHigh == (u32)(g_titleId >> 32)) {
                g_block = b;
                return;
            }
        }
    }

    // ---------------------------------------------------------------- settings file
    bool g_enabledAtStart = false;

    void SaveIni() {
        if (!g_block) return;
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/fsrSwitch", 0777);
        char path[96];
        snprintf(path, sizeof(path), "sdmc:/config/fsrSwitch/%016lX.ini", g_titleId);
        FILE* f = fopen(path, "w");
        if (!f) return;
        const u32 sharp = g_block->sharpness > 1000 ? 1000 : g_block->sharpness;
        const u32 flip = g_block->flip;
        const u32 aa = g_block->antialias > 1000 ? 1000 : g_block->antialias;
        fprintf(f,
            "# NX-FSR settings for this game (written by the overlay)\n"
            "# enabled: 1 = on, 0 = off\n"
            "enabled=%u\n"
            "# sharpness: 0.0 (soft) .. 1.0 (strong)\n"
            "sharpness=%u.%02u\n"
            "# antialias: 0.0 (off) .. 1.0 (strong)\n"
            "antialias=%u.%02u\n"
            "# split: 1 = effect only on the left half (for comparing), 0 = whole screen\n"
            "split=%u\n"
            "# flip: auto (follow the game), on, off. Use on/off if the picture is upside down\n"
            "flip=%s\n",
            g_block->enabled ? 1u : 0u, sharp / 1000, (sharp % 1000) / 10, aa / 1000, (aa % 1000) / 10,
            g_block->split ? 1u : 0u,
            flip == FSR_FLIP_ON ? "on" : (flip == FSR_FLIP_OFF ? "off" : "auto"));
        fclose(f);
    }

    std::string StateText() {
        if (!g_block->enabled) return T(T_ST_OFF);
        switch (g_block->state) {
            case FSR_STATE_RUNNING: return T(T_ST_RUNNING);
            case FSR_STATE_FAILED: return T(T_ST_FAILED);
            case FSR_STATE_NO_MEMORY: return T(T_ST_NO_MEMORY);
            default: return T(T_ST_WAITING);
        }
    }

    std::string FrameText() {
        if (!g_block->width) return "-";
        char buf[48];
        snprintf(buf, sizeof(buf), "%ux%u", g_block->width, g_block->height);
        return buf;
    }

    std::string GpuText() {
        const u32 ns = g_block->gpuTimeNs;
        if (!ns || g_block->state != FSR_STATE_RUNNING || !g_block->enabled) return "-";
        char buf[32];
        snprintf(buf, sizeof(buf), "%u.%02u ms", ns / 1000000, (ns / 10000) % 100);
        return buf;
    }

    std::string OriginText() {
        const u32 m = g_block->originMode;
        std::string s = m == 1 ? T(T_ORIENT_UPPER) : (m == 0 ? T(T_ORIENT_LOWER) : T(T_ORIENT_UNKNOWN));
        if (g_block->flipActive) s += " / flip";
        return s;
    }

    std::string PercentText(u32 v) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u%%", v / 10);
        return buf;
    }
}

class MessageGui : public tsl::Gui {
public:
    explicit MessageGui(const char* message) : m_message(message) {}

    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("NX-FSR", APP_VERSION);
        auto list = new tsl::elm::List();
        list->addItem(new tsl::elm::ListItem(m_message));
        frame->setContent(list);
        return frame;
    }

private:
    const char* m_message;
};

class MainGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("NX-FSR", APP_VERSION);
        auto list = new tsl::elm::List();

        auto effect = new tsl::elm::ToggleListItem(T(T_EFFECT), g_block->enabled != 0, T(T_ON), T(T_OFF));
        effect->setStateChangedListener([this](bool state) {
            g_block->enabled = state ? 1 : 0;
            SaveIni();
            if (m_noteItem) m_noteItem->setText(state && !g_enabledAtStart ? T(T_RESTART) : T(T_SAVED_NOTE));
        });
        list->addItem(effect);

        m_sharpItem = new tsl::elm::ListItem(T(T_SHARPNESS), PercentText(g_block->sharpness));
        list->addItem(m_sharpItem);
        auto sharp = new tsl::elm::StepTrackBar("", 11);
        sharp->setProgress((u8)((g_block->sharpness > 1000 ? 1000 : g_block->sharpness) / 100));
        sharp->setValueChangedListener([this](u8 step) {
            g_block->sharpness = (u32)step * 100;
            m_sharpItem->setValue(PercentText(g_block->sharpness));
            SaveIni();
        });
        list->addItem(sharp);

        m_aaItem = new tsl::elm::ListItem(T(T_ANTIALIAS), PercentText(g_block->antialias));
        list->addItem(m_aaItem);
        auto aa = new tsl::elm::StepTrackBar("", 11);
        aa->setProgress((u8)((g_block->antialias > 1000 ? 1000 : g_block->antialias) / 100));
        aa->setValueChangedListener([this](u8 step) {
            g_block->antialias = (u32)step * 100;
            m_aaItem->setValue(PercentText(g_block->antialias));
            SaveIni();
        });
        list->addItem(aa);

        auto split = new tsl::elm::ToggleListItem(T(T_COMPARE), g_block->split != 0, T(T_ON), T(T_OFF));
        split->setStateChangedListener([](bool state) {
            g_block->split = state ? 1 : 0;
            SaveIni();
        });
        list->addItem(split);

        list->addItem(new tsl::elm::CategoryHeader(std::string(T(T_FLIP)) + " - " + T(T_HINT_FLIP)));
        auto flip = new tsl::elm::NamedStepTrackBar("", {T(T_FLIP_AUTO), T(T_FLIP_OFF), T(T_FLIP_ON)});
        flip->setProgress((u8)(g_block->flip <= FSR_FLIP_ON ? g_block->flip : FSR_FLIP_AUTO));
        flip->setValueChangedListener([](u8 step) {
            g_block->flip = step;
            SaveIni();
        });
        list->addItem(flip);

        list->addItem(new tsl::elm::CategoryHeader(T(T_STATUS)));
        m_stateItem = new tsl::elm::ListItem(T(T_STATE), StateText());
        m_frameItem = new tsl::elm::ListItem(T(T_FRAME), FrameText());
        m_gpuItem = new tsl::elm::ListItem(T(T_GPU), GpuText());
        m_originItem = new tsl::elm::ListItem(T(T_ORIENTATION), OriginText());
        list->addItem(m_stateItem);
        list->addItem(m_frameItem);
        list->addItem(m_gpuItem);
        list->addItem(m_originItem);
        m_noteItem = new tsl::elm::ListItem(g_block->enabled && !g_enabledAtStart ? T(T_RESTART) : T(T_SAVED_NOTE));
        list->addItem(m_noteItem);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (++m_tick % 30) return; // twice a second
        m_stateItem->setValue(StateText());
        m_frameItem->setValue(FrameText());
        m_gpuItem->setValue(GpuText());
        m_originItem->setValue(OriginText());
    }

private:
    tsl::elm::ListItem* m_sharpItem = nullptr;
    tsl::elm::ListItem* m_aaItem = nullptr;
    tsl::elm::ListItem* m_stateItem = nullptr;
    tsl::elm::ListItem* m_frameItem = nullptr;
    tsl::elm::ListItem* m_gpuItem = nullptr;
    tsl::elm::ListItem* m_originItem = nullptr;
    tsl::elm::ListItem* m_noteItem = nullptr;
    u32 m_tick = 0;
};

class NxFsrOverlay : public tsl::Overlay {
public:
    void initServices() override {
        tsl::hlp::doWithSmSession([] {
            u64 languageCode = 0;
            SetLanguage language = SetLanguage_ENUS;
            if (R_SUCCEEDED(setInitialize())) {
                if (R_SUCCEEDED(setGetSystemLanguage(&languageCode))) setMakeLanguage(languageCode, &language);
                setExit();
            }
            g_russian = (language == SetLanguage_RU);

            fsdevMountSdmc();

            u64 pid = 0;
            if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid))) {
                g_gameRunning = true;
                if (R_SUCCEEDED(pminfoInitialize())) {
                    pminfoGetProgramId(&g_titleId, pid);
                    pminfoExit();
                }
            }

            if (R_SUCCEEDED(saltynx::Connect())) {
                g_saltyRunning = true;
                Handle handle = 0;
                const Result rc = saltynx::GetSharedMemoryHandle(&handle);
                saltynx::Term();
                if (R_SUCCEEDED(rc)) {
                    shmemLoadRemote(&g_shmem, handle, 0x1000, Perm_Rw);
                    g_shmemMapped = R_SUCCEEDED(shmemMap(&g_shmem));
                }
            }
            FindBlock();
            if (g_block) g_enabledAtStart = (g_block->state != FSR_STATE_NO_MEMORY);
        });
    }

    void exitServices() override {
        if (g_shmemMapped) shmemClose(&g_shmem);
        fsdevUnmountDevice("sdmc");
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        if (!g_gameRunning) return initially<MessageGui>(T(T_NO_GAME));
        if (!g_saltyRunning || !g_shmemMapped) return initially<MessageGui>(T(T_NO_SALTYNX));
        if (!g_block) return initially<MessageGui>(T(T_NO_BLOCK));
        return initially<MainGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<NxFsrOverlay>(argc, argv);
}

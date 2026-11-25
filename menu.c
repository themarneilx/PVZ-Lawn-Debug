#include <stddef.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "game.h"

enum Page { HOME, RESOURCES, GAMEPLAY, ZOMBIES, DIAGNOSTICS };
enum Item {
    OPEN_RESOURCES, OPEN_GAMEPLAY, OPEN_ZOMBIES, OPEN_DIAGNOSTICS, REMEMBER, CLOSE,
    SUN_AMOUNT, ADD_SUN, SET_SUN, COIN_AMOUNT, ADD_COINS,
    FREE, NO_COST, RECHARGE, CHOMPER_SWALLOW, UNLOCK_PLANTS, PROTECT, SPEED, PLANT_HEALTH, ZOMBIE_HEALTH,
    ZOMBIE_TYPE, ROW, COLUMN, SPAWN_ALLY, SPAWN_ENEMY, NEXT_WAVE,
    DISPLAY
};
typedef struct { enum Item item; const wchar_t *label; } Entry;

static const Entry home[] = {
    { OPEN_RESOURCES, L"Sun and coins" }, { OPEN_GAMEPLAY, L"Plants and game rules" },
    { OPEN_ZOMBIES, L"Zombies and waves" }, { OPEN_DIAGNOSTICS, L"Debug displays" },
    { REMEMBER, L"Keep settings between levels" },
    { CLOSE, L"Close menu" }
};
static const Entry resources[] = {
    { SUN_AMOUNT, L"Sun amount" }, { ADD_SUN, L"Add sun" }, { SET_SUN, L"Set sun" },
    { COIN_AMOUNT, L"Coin amount" }, { ADD_COINS, L"Add temporary coins" }
};
static const Entry gameplay[] = {
    { FREE, L"Free planting" }, { NO_COST, L"No sun cost" }, { RECHARGE, L"Instant recharge" },
    { CHOMPER_SWALLOW, L"Instant Chomper swallow" },
    { UNLOCK_PLANTS, L"Unlock all plants" }, { PROTECT, L"Invulnerable plants" }, { SPEED, L"Game speed" },
    { PLANT_HEALTH, L"Plant health" }, { ZOMBIE_HEALTH, L"Zombie body health" }
};
static const Entry spawning[] = {
    { ZOMBIE_TYPE, L"Zombie type" }, { ROW, L"Lane" }, { COLUMN, L"Ally column" },
    { SPAWN_ALLY, L"Plant allied zombie" }, { SPAWN_ENEMY, L"Spawn enemy at lawn edge" },
    { NEXT_WAVE, L"Trigger next wave" }
};
static const Entry diagnostics[] = { { DISPLAY, L"Board display" } };

#define PAGE(title, entries) { title, entries, sizeof(entries) / sizeof(entries[0]) }
static const struct { const wchar_t *title; const Entry *entries; int count; } pages[] = {
    PAGE(L"Lawn Debug", home), PAGE(L"Sun and coins", resources),
    PAGE(L"Plants and game rules", gameplay), PAGE(L"Zombies and waves", spawning),
    PAGE(L"Debug displays", diagnostics)
};

static const int sun_amounts[] = { 25, 50, 100, 250, 500, 1000, 9990 };
static const int coin_amounts[] = { 100, 500, 1000, 5000, 10000 };
static int sun_step = 2, coin_step = 2;
static int zombie_choice, row, column = 1;
static enum Page page;
static int selected;
static int home_selection;
static wchar_t feedback[128];
static wchar_t settings_path[MAX_PATH];
static const wchar_t settings_section[] = L"DebugRules";

static const wchar_t class_name[] = L"PvZLocalDebugPanel1051";
static HWND popup, owner;
static HHOOK key_hook;
static HFONT title_font, body_font, footer_font;
enum { WIDTH = 490, HEIGHT = 454, FIRST_ROW = 90, ROW_HEIGHT = 33 };
#define COUNT(array) ((int)(sizeof(array) / sizeof(array[0])))

static int ReadSetting(const wchar_t *key, int fallback, unsigned count) {
    unsigned value = GetPrivateProfileIntW(settings_section, key, fallback, settings_path);
    return value < count ? (int)value : fallback;
}

static void LoadSettings(HMODULE process) {
    DWORD length = GetModuleFileNameW(process, settings_path, COUNT(settings_path));
    if (length < 4 || length >= COUNT(settings_path)) {
        settings_path[0] = L'\0';
        return;
    }
    lstrcpyW(settings_path + length - 4, L".ini");
    remember_rules = ReadSetting(L"Keep", 0, 2) != 0;
    if (!remember_rules) return;

    unsigned mask = ReadSetting(L"Rules", 0, 1u << RULE_COUNT);
    for (int i = 0; i < RULE_COUNT; i++) {
        if (i != UNLOCK_ALL_PLANTS) rules.rule[i] = (mask & (1u << i)) != 0;
    }
    rules.speed = ReadSetting(L"Speed", 1, 5);
    rules.display = ReadSetting(L"Display", 0, 5);
    rules.plant_health = ReadSetting(L"PlantHealth", DEFAULT_HEALTH_STEP, 5);
    rules.zombie_health = ReadSetting(L"ZombieHealth", DEFAULT_HEALTH_STEP, 5);
    SetRule(UNLOCK_ALL_PLANTS, (mask & (1u << UNLOCK_ALL_PLANTS)) != 0);
}

static bool SaveSettings(void) {
    if (!settings_path[0]) return false;
    if (!remember_rules)
        return WritePrivateProfileSectionW(settings_section, L"Keep=0\0", settings_path) != 0;

    unsigned mask = 0;
    for (int i = 0; i < RULE_COUNT; i++) {
        if (rules.rule[i]) mask |= 1u << i;
    }
    wchar_t data[128];
    int length = wsprintfW(data, L"Keep=1\nRules=%u\nSpeed=%d\nDisplay=%d\nPlantHealth=%d\nZombieHealth=%d",
                           mask, rules.speed, rules.display, rules.plant_health, rules.zombie_health);
    for (int i = 0; i < length; i++)
        if (data[i] == L'\n') data[i] = L'\0';
    data[length + 1] = L'\0';
    return WritePrivateProfileSectionW(settings_section, data, settings_path) != 0;
}

static void ClosePanel(void) {
    panel_open = false;
    if (popup) ShowWindow(popup, SW_HIDE);
}

static int MoveIndex(int value, int step, int count) {
    return (value + step + count) % count;
}

static bool Enabled(enum Item item) {
    switch (item) {
        case ADD_SUN: case SET_SUN: case PLANT_HEALTH: case ZOMBIE_HEALTH:
            return InBattle();
        case ADD_COINS: return HasWallet();
        case SPAWN_ALLY: case SPAWN_ENEMY: return CanSpawn(zombie_choice, row);
        case NEXT_WAVE: return HasNextWave();
        default: return true;
    }
}

static const wchar_t *Hint(enum Item item) {
    switch (item) {
        case FREE: return L"Bypasses both sun cost and seed recharge checks.";
        case NO_COST: return L"Seed packets still recharge at their usual rate.";
        case RECHARGE: return L"Seeds are ready on the next game tick.";
        case CHOMPER_SWALLOW: return L"Chompers skip chewing after a bite.";
        case UNLOCK_PLANTS: return L"Makes every plant available in Choose Your Plants.";
        case REMEMBER: return L"Save rules for new levels and game launches.";
        case PROTECT: return L"Blocks bites, crushing, and common sources of plant damage.";
        case ADD_COINS: return HasWallet() ? L"Unused debug coins expire with this board." :
                                            L"Open a level or the Zen Garden to add coins.";
        case PLANT_HEALTH: case ZOMBIE_HEALTH:
            return InBattle() ? L"Applies immediately to current units." :
                               L"Available during a playable battle.";
        case SPAWN_ALLY: case SPAWN_ENEMY:
            return Enabled(item) ? (item == SPAWN_ALLY ? L"The ally walks right and attacks enemy zombies."
                                                        : L"The enemy enters from the right.") :
                   InBattle() ? L"Pick an active lane and a zombie suited to its terrain." :
                                L"Available during a playable battle.";
        case NEXT_WAVE: return HasNextWave() ? L"Starts the next wave using the game's normal wave logic." :
                                               L"No upcoming wave on this board.";
        case ADD_SUN: case SET_SUN: return InBattle() ? L"Applies to the sun balance on this board." :
                                                     L"Available during a playable battle.";
        case DISPLAY: return L"Choose one of the game's built-in board displays.";
        case CLOSE: return L"Return to the game.";
        default: return L"";
    }
}

static const wchar_t *Value(enum Item item, wchar_t *buffer) {
    switch (item) {
        case OPEN_RESOURCES: case OPEN_GAMEPLAY: case OPEN_ZOMBIES: case OPEN_DIAGNOSTICS:
            return L"\u203a";
        case SUN_AMOUNT: wsprintfW(buffer, L"%d", sun_amounts[sun_step]); break;
        case COIN_AMOUNT: wsprintfW(buffer, L"$%d", coin_amounts[coin_step]); break;
        case FREE: return rules.rule[FREE_PLANTING] ? L"On" : L"Off";
        case NO_COST: return rules.rule[NO_SUN_COST] ? L"On" : L"Off";
        case RECHARGE: return rules.rule[INSTANT_RECHARGE] ? L"On" : L"Off";
        case CHOMPER_SWALLOW: return rules.rule[INSTANT_CHOMPER_SWALLOW] ? L"On" : L"Off";
        case UNLOCK_PLANTS: return rules.rule[UNLOCK_ALL_PLANTS] ? L"On" : L"Off";
        case REMEMBER: return remember_rules ? L"On" : L"Off";
        case PROTECT: return rules.rule[PLANT_PROTECTION] ? L"On" : L"Off";
        case SPEED: return speed_names[rules.speed];
        case PLANT_HEALTH: wsprintfW(buffer, L"%d%%", health_percents[rules.plant_health]); break;
        case ZOMBIE_HEALTH: wsprintfW(buffer, L"%d%%", health_percents[rules.zombie_health]); break;
        case ZOMBIE_TYPE: return zombies[zombie_choice].name;
        case ROW: wsprintfW(buffer, L"%d", row + 1); break;
        case COLUMN: wsprintfW(buffer, L"%d", column + 1); break;
        case DISPLAY: return display_names[rules.display];
        default: return L"";
    }
    return buffer;
}

static void Toggle(enum Rule rule) {
    bool enable = !rules.rule[rule];
    if (rule == UNLOCK_ALL_PLANTS && !CanChangePlantUnlock(enable)) {
        lstrcpyW(feedback, enable ? L"Wait for your plant cards to finish moving." :
                                   L"Remove your selected plants to turn this off here.");
        return;
    }
    if (!SetRule(rule, enable))
        lstrcpyW(feedback, L"This rule is unavailable for the current executable.");
    else if (remember_rules && !SaveSettings())
        lstrcpyW(feedback, L"Applied here. Could not save debug settings to disk.");
}

static bool Change(enum Item item, int step) {
    if (!Enabled(item)) return false;
    switch (item) {
        case SUN_AMOUNT: sun_step = MoveIndex(sun_step, step, COUNT(sun_amounts)); return true;
        case COIN_AMOUNT: coin_step = MoveIndex(coin_step, step, COUNT(coin_amounts)); return true;
        case PLANT_HEALTH: case ZOMBIE_HEALTH: {
            bool plants = item == PLANT_HEALTH;
            int *index = plants ? &rules.plant_health : &rules.zombie_health;
            *index = MoveIndex(*index, step, 5);
            int units = SetHealth(plants, health_percents[*index]);
            wsprintfW(feedback, plants ? L"Updated %d plants." : L"Updated %d zombies.", units);
            break;
        }
        case ZOMBIE_TYPE: zombie_choice = MoveIndex(zombie_choice, step, zombie_choice_count); return true;
        case ROW: row = MoveIndex(row, step, 6); return true;
        case COLUMN: column = MoveIndex(column, step, 9); return true;
        case SPEED: rules.speed = MoveIndex(rules.speed, step, 5); ApplyRules(); break;
        case DISPLAY: rules.display = MoveIndex(rules.display, step, 5); ApplyRules(); break;
        case FREE: Toggle(FREE_PLANTING); return true;
        case NO_COST: Toggle(NO_SUN_COST); return true;
        case RECHARGE: Toggle(INSTANT_RECHARGE); return true;
        case CHOMPER_SWALLOW: Toggle(INSTANT_CHOMPER_SWALLOW); return true;
        case UNLOCK_PLANTS: Toggle(UNLOCK_ALL_PLANTS); return true;
        case PROTECT: Toggle(PLANT_PROTECTION); return true;
        case REMEMBER:
            remember_rules = !remember_rules;
            if (!SaveSettings()) {
                remember_rules = !remember_rules;
                lstrcpyW(feedback, L"Could not save debug settings to disk.");
            }
            return true;
        default: return false;
    }
    if (remember_rules && !SaveSettings())
        lstrcpyW(feedback, L"Applied here. Could not save debug settings to disk.");
    return true;
}

static void Activate(enum Item item) {
    if (!Enabled(item) || Change(item, 1)) return;
    int units;
    switch (item) {
        case OPEN_RESOURCES: case OPEN_GAMEPLAY: case OPEN_ZOMBIES: case OPEN_DIAGNOSTICS:
            home_selection = selected;
            page = (enum Page)(RESOURCES + item - OPEN_RESOURCES);
            selected = 0;
            break;
        case CLOSE: ClosePanel(); break;
        case ADD_SUN: AddSun(sun_amounts[sun_step]); wsprintfW(feedback, L"Sun balance is now %d.", Sun()); break;
        case SET_SUN: SetSun(sun_amounts[sun_step]); wsprintfW(feedback, L"Sun balance set to %d.", Sun()); break;
        case ADD_COINS:
            units = AddCoins(coin_amounts[coin_step]);
            wsprintfW(feedback, L"Added $%d in temporary coins.", units);
            break;
        case SPAWN_ALLY: case SPAWN_ENEMY:
            if (SpawnZombie(zombie_choice, row, column, item == SPAWN_ALLY))
                wsprintfW(feedback, L"%s added to lane %d.", zombies[zombie_choice].name, row + 1);
            else
                lstrcpyW(feedback, L"Could not spawn a zombie in this lane.");
            break;
        case NEXT_WAVE:
            AdvanceWave();
            lstrcpyW(feedback, L"The next wave will start when you resume.");
            break;
        default: break;
    }
}

static void HandleKey(WPARAM key, bool held) {
    feedback[0] = 0;
    switch (key) {
        case VK_UP: selected = MoveIndex(selected, -1, pages[page].count); break;
        case VK_DOWN: selected = MoveIndex(selected, 1, pages[page].count); break;
        case VK_LEFT: case VK_RIGHT:
            if (!held) Change(pages[page].entries[selected].item, key == VK_LEFT ? -1 : 1);
            break;
        case VK_RETURN:
            if (!held) Activate(pages[page].entries[selected].item);
            break;
        case VK_ESCAPE:
            if (held) break;
            if (page == HOME) ClosePanel();
            else { page = HOME; selected = home_selection; }
            break;
        default: break;
    }
    if (popup) InvalidateRect(popup, NULL, FALSE);
}

static void Fill(HDC dc, int x, int y, int width, int height, COLORREF color) {
    RECT rect = { x, y, x + width, y + height };
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

static void Text(HDC dc, const wchar_t *text, HFONT font, COLORREF color,
                 int x, int y, int width, int height, UINT align) {
    RECT rect = { x, y, x + width, y + height };
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void DrawPanel(HDC dc) {
    const COLORREF foreground = RGB(239, 242, 227);
    const COLORREF muted = RGB(172, 187, 162);
    const COLORREF accent = RGB(210, 237, 153);
    Fill(dc, 0, 0, WIDTH, HEIGHT, RGB(26, 37, 28));
    Fill(dc, 0, 0, WIDTH, 4, accent);
    SetBkMode(dc, TRANSPARENT);
    Text(dc, pages[page].title, title_font, foreground, 22, 13, WIDTH - 44, 36, DT_LEFT);

    const wchar_t *context = InBattle() ? L"Level paused" :
                             HasBoard() ? L"Board paused" : L"Rules apply to the next board";
    wchar_t balance[128];
    if (page == RESOURCES && HasWallet()) {
        if (InBattle()) wsprintfW(balance, L"Sun %d    Coins $%d", Sun(), Coins());
        else wsprintfW(balance, L"Coins $%d", Coins());
        context = balance;
    }
    Text(dc, context, footer_font, muted, 22, 51, WIDTH - 44, 24, DT_LEFT);
    Fill(dc, 22, 79, WIDTH - 44, 1, RGB(68, 86, 63));

    for (int i = 0; i < pages[page].count; i++) {
        const Entry *entry = &pages[page].entries[i];
        int y = FIRST_ROW + i * ROW_HEIGHT;
        if (i == selected) {
            Fill(dc, 13, y, WIDTH - 26, ROW_HEIGHT - 3, RGB(56, 78, 45));
            Fill(dc, 13, y, 3, ROW_HEIGHT - 3, accent);
        }
        COLORREF tone = Enabled(entry->item) ? foreground : muted;
        Text(dc, entry->label, body_font, tone, 25, y, 269, ROW_HEIGHT - 3, DT_LEFT);
        wchar_t buffer[32];
        Text(dc, Value(entry->item, buffer), body_font, accent,
             292, y, WIDTH - 321, ROW_HEIGHT - 3, DT_RIGHT);
    }

    const wchar_t *status = feedback[0] ? feedback : Hint(pages[page].entries[selected].item);
    Text(dc, status, footer_font, feedback[0] ? accent : muted, 22, 386, WIDTH - 44, 28, DT_LEFT);
    Fill(dc, 22, 419, WIDTH - 44, 1, RGB(68, 86, 63));
    Text(dc, L"\u2191\u2193 Move    \u2190\u2192 Change    Enter Select    Esc Back    F9 Close",
         footer_font, muted, 22, 423, WIDTH - 44, 24, DT_LEFT);
}

static LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATEANDEAT;
        case WM_CLOSE: ClosePanel(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, WIDTH, HEIGHT) : NULL;
            if (bitmap) {
                HGDIOBJ previous = SelectObject(memory, bitmap);
                DrawPanel(memory);
                BitBlt(dc, 0, 0, WIDTH, HEIGHT, memory, 0, 0, SRCCOPY);
                SelectObject(memory, previous);
                DeleteObject(bitmap);
            } else {
                DrawPanel(dc);
            }
            if (memory) DeleteDC(memory);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_TIMER:
            if (!IsWindow(owner) || GetForegroundWindow() != owner) ClosePanel();
            return 0;
        case WM_SHOWWINDOW:
            if (wparam) SetTimer(window, 1, 200, NULL);
            else KillTimer(window, 1);
            return 0;
        case WM_DESTROY:
            popup = NULL;
            panel_open = false;
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static void OpenPanel(HWND game_window) {
    if (!game_window) return;
    if (popup && owner != game_window) DestroyWindow(popup);
    owner = game_window;
    if (!popup) {
        popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name, L"Lawn Debug",
                                WS_POPUP, 0, 0, WIDTH, HEIGHT, owner, NULL,
                                GetModuleHandleW(NULL), NULL);
        if (!popup) return;
    }
    RECT bounds;
    if (!GetWindowRect(owner, &bounds)) return;
    int x = (bounds.left + bounds.right - WIDTH) / 2;
    int y = (bounds.top + bounds.bottom - HEIGHT) / 2;
    page = HOME;
    selected = 0;
    feedback[0] = 0;
    panel_open = SetWindowPos(popup, HWND_TOPMOST, x, y, WIDTH, HEIGHT, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(popup, NULL, FALSE);
}

static LRESULT CALLBACK KeyFilter(int code, WPARAM remove, LPARAM data) {
    if (code >= 0 && remove == PM_REMOVE) {
        MSG *event = (MSG *)data;
        bool held = (event->lParam & 0x40000000) != 0;
        if (event->message == WM_KEYDOWN && event->wParam == VK_F9) {
            if (!held) {
                if (panel_open) ClosePanel();
                else OpenPanel(GetAncestor(event->hwnd, GA_ROOT));
            }
            event->message = WM_NULL;
        } else if (panel_open) {
            if (event->message == WM_KEYDOWN) {
                HandleKey(event->wParam, held);
                event->message = WM_NULL;
            } else if (event->message == WM_KEYUP || event->message == WM_CHAR ||
                       (event->message >= WM_MOUSEFIRST && event->message <= WM_MOUSELAST)) {
                event->message = WM_NULL;
            }
        } else if (event->message == WM_KEYDOWN && event->wParam == VK_ESCAPE &&
                   !held && CancelZombieSelection()) {
            event->message = WM_NULL;
        }
    }
    return CallNextHookEx(key_hook, code, remove, data);
}

void PvzDebugInit(void) {
    HMODULE process = GetModuleHandleW(NULL);
    if (process != (HMODULE)0x400000) return;
    LoadSettings(process);
    WNDCLASSW window_class = {0};
    window_class.lpfnWndProc = PanelProc;
    window_class.hInstance = process;
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) return;

    title_font = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    body_font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    footer_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    key_hook = SetWindowsHookExW(WH_GETMESSAGE, KeyFilter, NULL, GetCurrentThreadId());
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

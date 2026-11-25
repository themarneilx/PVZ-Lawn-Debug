#include <stddef.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "game.h"

typedef unsigned char Byte;
#define FIELD(type, object, offset) (*(type *)((object) + (offset)))

enum {
    APP_TICK_MS = 0x454, APP_BOARD = 0x768, APP_MODE = 0x7f8,
    APP_SCENE = 0x7fc, APP_FREE_PLANTING = 0x814, APP_PROFILE = 0x82c,
    PROFILE_COINS = 0x28,
    BOARD_ZOMBIES = 0x90, BOARD_PLANTS = 0xac, BOARD_SEED_BANK = 0x144,
    BOARD_ROW_TYPES = 0x5d8, BOARD_SUN = 0x5560, BOARD_WAVE_COUNT = 0x5564,
    BOARD_CURRENT_WAVE = 0x557c, BOARD_WAVE_TIMER = 0x559c,
    BOARD_DEBUG_DISPLAY = 0x55f8,
    SCENE_PLAYING = 3, MODE_ZEN_GARDEN = 43, MODE_TREE_OF_WISDOM = 50,
    WALLET_LIMIT = 99999, SUN_LIMIT = 9990, MAX_ENTITIES = 1024
};

#define DEFAULT_RULES { .speed = 1, .plant_health = DEFAULT_HEALTH_STEP, .zombie_health = DEFAULT_HEALTH_STEP }
Rules rules = DEFAULT_RULES;
bool panel_open;
bool remember_rules;
const wchar_t *const speed_names[] = { L"0.5x", L"1x", L"2x", L"3.3x", L"5x" };
const wchar_t *const display_names[] = {
    L"Off", L"Wave & spawn timing", L"Music", L"Memory & objects", L"Collisions"
};
const int health_percents[] = { 25, 50, 100, 150, 200 };

const ZombieChoice zombies[] = {
    { L"Browncoat", 0, false },
    { L"Conehead", 2, false },
    { L"Buckethead", 4, false },
    { L"Newspaper", 5, false },
    { L"Screen door", 6, false },
    { L"Football", 7, false },
    { L"Ducky tube", 10, true },
    { L"Pole vault", 3, false }
};
const int zombie_choice_count = sizeof(zombies) / sizeof(zombies[0]);
static const int tick_ms[] = { 20, 10, 5, 3, 2 };

static Byte *last_board;
static Byte *credit_profile;
static int credit;
static int last_balance;
static unsigned seen_units[2][MAX_ENTITIES];
static int seen_slots[2];

typedef struct {
    uintptr_t address;
    Byte size;
    Byte original[3];
    Byte enabled[3];
} CodeChange;

static const CodeChange free_cost[] = {
    { 0x41dae0, 3, {0x53, 0x56, 0x8b}, {0x31, 0xc0, 0xc3} }
};
// Skip the chewing timer check so the game runs its normal swallow transition.
static const CodeChange chomper_swallow = { 0x461565, 2, {0x75, 0x5f}, {0x90, 0x90} };
static const CodeChange protection[] = {
    { 0x52fcf3, 1, {0xfc}, {0x00} },
    { 0x41cc2f, 1, {0x74}, {0xeb} },
    { 0x5276ea, 1, {0x75}, {0xeb} },
    { 0x46cfeb, 3, {0x29, 0x50, 0x40}, {0x90, 0x90, 0x90} },
    { 0x46d7a6, 3, {0x29, 0x4e, 0x40}, {0x90, 0x90, 0x90} },
    { 0x52e93b, 1, {0x74}, {0xeb} },
    { 0x45ee0a, 1, {0x75}, {0x70} },
    { 0x45ec66, 1, {0xce}, {0x00} },
    { 0x462b80, 3, {0x53, 0x55, 0x8b}, {0xc2, 0x04, 0x00} }
};
// These picker checks merge ordinary unlocks with the separate Gatling Pea check.
#define ALLOW_SEED(address) { address, 2, {0x84, 0xc0}, {0x0c, 0x01} }
static const CodeChange unlock_plants[] = {
    ALLOW_SEED(0x482d62), ALLOW_SEED(0x482f0e),
    ALLOW_SEED(0x484007), ALLOW_SEED(0x484873), ALLOW_SEED(0x48498b),
    ALLOW_SEED(0x484b26), ALLOW_SEED(0x485222), ALLOW_SEED(0x485a60),
    ALLOW_SEED(0x485dc2), ALLOW_SEED(0x485e46),
    { 0x48423a, 2, {0x7e, 0x07}, {0x90, 0x90} }, // Use the full chooser grid.
    { 0x484772, 3, {0x0f, 0x9f, 0xc0}, {0xb0, 0x01, 0x90} },
    { 0x486724, 1, {0x75}, {0xeb} } // Enable the separate Imitater button.
};
#undef ALLOW_SEED
static bool free_cost_active;
static bool chomper_swallow_active;
static bool protection_active;
static bool unlock_plants_active;

static int ApplyHealth(bool plants, int percent, bool new_only);

static Byte *App(void) {
    return *(Byte **)0x6a9ec0;
}

static Byte *Board(void) {
    Byte *app = App();
    return app ? FIELD(Byte *, app, APP_BOARD) : NULL;
}

static Byte *Profile(void) {
    Byte *app = App();
    return app ? FIELD(Byte *, app, APP_PROFILE) : NULL;
}

static int Clamp(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

bool HasBoard(void) {
    return Board() != NULL;
}

bool InBattle(void) {
    Byte *app = App();
    Byte *board = Board();
    if (!board || FIELD(int, app, APP_SCENE) != SCENE_PLAYING) return false;
    int mode = FIELD(int, app, APP_MODE);
    return mode != MODE_ZEN_GARDEN && mode != MODE_TREE_OF_WISDOM;
}

bool HasWallet(void) {
    return HasBoard() && Profile() != NULL;
}

int Sun(void) {
    return HasBoard() ? FIELD(int, Board(), BOARD_SUN) : 0;
}

int Coins(void) {
    Byte *profile = Profile();
    return profile ? FIELD(int, profile, PROFILE_COINS) * 10 : 0;
}

void SetSun(int amount) {
    if (InBattle()) FIELD(int, Board(), BOARD_SUN) = Clamp(amount, 0, SUN_LIMIT);
}

void AddSun(int amount) {
    SetSun(Sun() + amount);
}

static void ReconcileCredit(Byte *profile, int balance) {
    if (profile != credit_profile) return;
    int spent = last_balance - balance;
    if (spent > 0) credit = Clamp(credit - spent, 0, credit);
    last_balance = balance;
}

int AddCoins(int dollars) {
    Byte *profile = Profile();
    if (!HasWallet()) return 0;
    int balance = FIELD(int, profile, PROFILE_COINS);
    if (profile != credit_profile) {
        credit_profile = profile;
        credit = 0;
        last_balance = balance;
    }
    ReconcileCredit(profile, balance);
    int addition = Clamp(dollars / 10, 0, WALLET_LIMIT - balance);
    FIELD(int, profile, PROFILE_COINS) = balance + addition;
    credit += addition;
    last_balance = balance + addition;
    return addition * 10;
}

int PvzDebugWalletForSave(Byte *profile, int balance) {
    ReconcileCredit(profile, balance);
    return profile == credit_profile ? Clamp(balance - credit, 0, WALLET_LIMIT) : balance;
}

static bool WriteChange(const CodeChange *change, bool enabled) {
    const Byte *bytes = enabled ? change->enabled : change->original;
    Byte *address = (Byte *)change->address;
    DWORD old_protection;
    if (!VirtualProtect(address, change->size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    for (int i = 0; i < change->size; i++) address[i] = bytes[i];
    FlushInstructionCache(GetCurrentProcess(), address, change->size);
    VirtualProtect(address, change->size, old_protection, &old_protection);
    return true;
}

static bool SetChanges(const CodeChange *changes, int count, bool *active, bool enable) {
    if (*active == enable) return true;
    for (int i = 0; i < count; i++) {
        const CodeChange *change = &changes[i];
        const Byte *before = *active ? change->enabled : change->original;
        for (int b = 0; b < change->size; b++)
            if (((const Byte *)change->address)[b] != before[b]) return false;
    }
    for (int i = 0; i < count; i++) {
        if (!WriteChange(&changes[i], enable)) {
            while (i > 0) WriteChange(&changes[--i], *active);
            return false;
        }
    }
    *active = enable;
    return true;
}

static bool ApplyCodeRules(bool has_board) {
    bool cost = SetChanges(free_cost, 1, &free_cost_active,
                          has_board && rules.rule[NO_SUN_COST]);
    bool chomp = SetChanges(&chomper_swallow, 1, &chomper_swallow_active,
                            has_board && rules.rule[INSTANT_CHOMPER_SWALLOW]);
    bool plants = SetChanges(protection, sizeof(protection) / sizeof(protection[0]),
                            &protection_active, has_board && rules.rule[PLANT_PROTECTION]);
    bool unlock = SetChanges(unlock_plants, sizeof(unlock_plants) / sizeof(unlock_plants[0]),
                            &unlock_plants_active, rules.rule[UNLOCK_ALL_PLANTS]);
    return cost && chomp && plants && unlock;
}

void ApplyRules(void) {
    Byte *app = App();
    Byte *board = Board();
    if (!app || !board) return;
    FIELD(Byte, app, APP_FREE_PLANTING) = rules.rule[FREE_PLANTING];
    FIELD(int, app, APP_TICK_MS) = tick_ms[rules.speed];
    FIELD(int, board, BOARD_DEBUG_DISPLAY) = rules.display;
}

bool SetRule(enum Rule rule, bool enabled) {
    bool previous = rules.rule[rule];
    rules.rule[rule] = enabled;
    if (!ApplyCodeRules(HasBoard())) {
        rules.rule[rule] = previous;
        ApplyCodeRules(HasBoard());
        return false;
    }
    ApplyRules();
    if (rule == UNLOCK_ALL_PLANTS && previous != enabled) RefreshPlantPicker();
    return true;
}

void PvzDebugEndBoard(Byte *app) {
    Byte *board = FIELD(Byte *, app, APP_BOARD);
    if (!board) return;
    Byte *profile = FIELD(Byte *, app, APP_PROFILE);
    if (profile && profile == credit_profile) {
        int balance = FIELD(int, profile, PROFILE_COINS);
        FIELD(int, profile, PROFILE_COINS) = PvzDebugWalletForSave(profile, balance);
    }
    credit_profile = NULL;
    credit = 0;
    last_board = NULL;
    if (!remember_rules) rules = (Rules)DEFAULT_RULES;
    seen_slots[0] = seen_slots[1] = 0;
    ResetZombiePicker();
    ApplyCodeRules(false);
    FIELD(Byte, app, APP_FREE_PLANTING) = 0;
    FIELD(int, app, APP_TICK_MS) = tick_ms[1];
    FIELD(int, board, BOARD_DEBUG_DISPLAY) = 0;
}

static void RefreshSeedPackets(Byte *board) {
    Byte *bank = FIELD(Byte *, board, BOARD_SEED_BANK);
    if (!bank) return;
    int slots = Clamp(FIELD(int, bank, 0x24), 0, 10);
    for (int i = 0; i < slots; i++) {
        Byte *slot = bank + 0x28 + i * 0x50;
        if (FIELD(Byte, slot, 0x49) && FIELD(int, slot, 0x34) >= 0) {
            FIELD(int, slot, 0x24) = FIELD(int, slot, 0x28);
        }
    }
}

int PvzDebugUpdate(Byte *board) {
    if (board != Board()) return 0;
    if (board != last_board) {
        last_board = board;
        ApplyCodeRules(true);
        ApplyRules();
    }
    Byte *profile = Profile();
    if (profile) ReconcileCredit(profile, FIELD(int, profile, PROFILE_COINS));
    if (!panel_open && InBattle()) {
        if (rules.rule[INSTANT_RECHARGE]) RefreshSeedPackets(board);
        if (remember_rules && rules.plant_health != DEFAULT_HEALTH_STEP)
            ApplyHealth(true, health_percents[rules.plant_health], true);
        if (remember_rules && rules.zombie_health != DEFAULT_HEALTH_STEP)
            ApplyHealth(false, health_percents[rules.zombie_health], true);
    }
    return panel_open;
}

bool HasNextWave(void) {
    Byte *board = Board();
    return InBattle() && FIELD(int, board, BOARD_CURRENT_WAVE) < FIELD(int, board, BOARD_WAVE_COUNT);
}

void AdvanceWave(void) {
    if (HasNextWave()) FIELD(int, Board(), BOARD_WAVE_TIMER) = 1;
}

bool CanSpawn(int choice, int row) {
    if (!InBattle() || row < 0 || row >= 6 || choice < 0 || choice >= zombie_choice_count)
        return false;
    int terrain = FIELD(int, Board(), BOARD_ROW_TYPES + 4 * row);
    return terrain > 0 && zombies[choice].aquatic == (terrain == 2);
}

__attribute__((naked, noinline))
static Byte *AddZombie(Byte *board __attribute__((unused)), int type __attribute__((unused)),
                       int row __attribute__((unused)), int wave __attribute__((unused))) {
    __asm__(
        "pushl %ebx\n\t"
        "movl 8(%esp), %eax\n\t"
        "movl 20(%esp), %ebx\n\t"
        "pushl 16(%esp)\n\t"
        "pushl 16(%esp)\n\t"
        "movl $0x40ddc0, %ecx\n\t"
        "call *%ecx\n\t"
        "popl %ebx\n\t"
        "ret"
    );
}

static void MakeAlly(Byte *zombie) {
    __asm__ volatile(
        "movl $0x52fa60, %%eax\n\t"
        "call *%%eax"
        : "+S"(zombie)
        :
        : "eax", "ecx", "edx", "cc", "memory"
    );
}

static float ZombieY(Byte *zombie, int row) {
    float y;
    __asm__ volatile(
        "pushl %[row]\n\t"
        "movl $0x531880, %%ecx\n\t"
        "call *%%ecx\n\t"
        "fstps %[y]"
        : [y] "=m"(y), "+a"(zombie)
        : [row] "r"(row)
        : "ecx", "edx", "cc", "memory"
    );
    return y;
}

bool SpawnZombie(int choice, int row, int column, bool ally) {
    if (!CanSpawn(choice, row)) return false;
    Byte *board = Board();
    int wave = FIELD(int, board, BOARD_CURRENT_WAVE) - 1;
    Byte *zombie = AddZombie(board, zombies[choice].type, row, Clamp(wave, 0, 99));
    if (!zombie) return false;
    if (ally) {
        int x = 20 + 80 * Clamp(column, 0, 8);
        FIELD(float, zombie, 0x2c) = (float)x;
        FIELD(int, zombie, 0x08) = x;
        float y = ZombieY(zombie, row);
        FIELD(float, zombie, 0x30) = y;
        FIELD(int, zombie, 0x0c) = (int)y;
        MakeAlly(zombie);
    }
    return true;
}

static int ApplyHealth(bool plants, int percent, bool new_only) {
    Byte *board = Board();
    int array = plants ? BOARD_PLANTS : BOARD_ZOMBIES;
    Byte *entities = FIELD(Byte *, board, array);
    if (!entities) return 0;
    int count = Clamp(FIELD(int, board, array + 4), 0, MAX_ENTITIES);
    int group = plants ? 0 : 1;
    int size = plants ? 0x14c : 0x15c;
    int health = plants ? 0x40 : 0xc8;
    int dead = plants ? 0x141 : 0xec;
    int changed = 0;
    for (int i = 0; i < count; i++) {
        Byte *entry = entities + size * i;
        unsigned id = FIELD(unsigned, entry, size - 4);
        if (!(id >> 16) || FIELD(Byte, entry, dead) || FIELD(int, entry, health) <= 0) continue;
        if (new_only && i < seen_slots[group] && seen_units[group][i] == id) continue;
        int max_hp = FIELD(int, entry, health + 4);
        FIELD(int, entry, health) = Clamp(Clamp(max_hp, 1, 200000) * percent / 100, 1, 400000);
        seen_units[group][i] = id;
        changed++;
    }
    seen_slots[group] = count;
    return changed;
}

int SetHealth(bool plants, int percent) {
    return InBattle() ? ApplyHealth(plants, percent, false) : 0;
}

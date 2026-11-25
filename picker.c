#include <stddef.h>
#include <stdint.h>
#include "game.h"

typedef unsigned char Byte;
#define FIELD(type, object, offset) (*(type *)((object) + (offset)))
#define COUNT(array) ((int)(sizeof(array) / sizeof(array[0])))
#define NATIVE_NAKED __attribute__((naked, noinline))
#define UNUSED __attribute__((unused))

enum {
    BANK_BOARD = 0x04, BANK_X = 0x08, BANK_Y = 0x0c, BANK_VISIBLE = 0x18,
    BANK_PACKET_COUNT = 0x24, BANK_PACKET_START = 0x28,
    BOARD_APP = 0x8c, BOARD_BANK = 0x144, BOARD_IGNORE_MOUSE_UP = 0x150,
    BOARD_PAUSED = 0x164, BOARD_BACKGROUND = 0x554c, BOARD_LOW_SUN_FLASH = 0x5578,
    BOARD_CUTSCENE = 0x15c, BOARD_TIME_STOP = 0x5748,
    CUTSCENE_PRELOADED = 0x34, APP_BOARD = 0x768, APP_CHOOSER = 0x774,
    APP_MODE = 0x7f8, APP_SCENE = 0x7fc, SCENE_INTRO = 2,
    CHOOSER_X = 0x30, CHOOSER_Y = 0x34, CHOOSER_APP = 0xd10, CHOOSER_BOARD = 0xd14,
    CHOOSER_IN_FLIGHT = 0xd20, CHOOSER_IN_BANK = 0xd24, CHOOSER_STATE = 0xd38,
    GRAPHICS_CLIP = 0x20, GRAPHICS_COLOR = 0x30,
    SWITCH_X = 680, SWITCH_Y = 87, SWITCH_W = 109, SWITCH_H = 24,
    CARD_X = 85, CARD_Y = 7, CARD_STEP = 54, CARD_W = 50, CARD_H = 70,
    PICKER_X = 18, PICKER_Y = 92, PICKER_W = 430, PICKER_H = 451,
    PICKER_CARD_X = 31, PICKER_CARD_Y = 132, PICKER_CARD_STEP = 65
};

typedef struct { int r, g, b, a; } Color;
typedef struct { void *object; int type; } Hit;
typedef struct { int reserved; char chars[16]; unsigned size, capacity; } GameString;
typedef struct { const char *name; int seed; int choice; } Card;

static const Card cards[] = {
    { "Browncoat", 0x3c, 0 }, { "Conehead", 0x3d, 1 },
    { "Pole vault", 0x3e, 7 }, { "Buckethead", 0x3f, 2 },
    { "Screen door", 0x45, 4 }, { "Football", 0x43, 5 }
};
static const Color ink = { 247, 239, 212, 255 };
static const Color rim = { 190, 165, 108, 255 };
static const Color off = { 49, 42, 37, 255 };
static const Color active = { 62, 50, 85, 255 };
static const Color selected = { 247, 211, 108, 255 };
static const Color wood = { 87, 44, 26, 255 };
static const Color wood_inset = { 63, 31, 20, 255 };
static const unsigned all_cards = (1u << COUNT(cards)) - 1;
static unsigned equipped = (1u << COUNT(cards)) - 1;
static bool chooser_zombies_open;
static bool zombies_open;
static int held_card = -1;
static const char *status;

void ResetZombiePicker(void) {
    chooser_zombies_open = false;
    zombies_open = false;
    equipped = all_cards;
    held_card = -1;
    status = NULL;
}

bool CancelZombieSelection(void) {
    if (!zombies_open || held_card < 0) return false;
    held_card = -1;
    status = NULL;
    return true;
}

static int NativePrice(int seed) {
    __asm__ volatile(
        "movl $-1, %%edx\n\t"
        "movl $0x467b00, %%ecx\n\t"
        "call *%%ecx"
        : "+a"(seed) : : "edx", "ecx", "cc", "memory"
    );
    return seed;
}

static int Price(int card) {
    if (rules.rule[FREE_PLANTING] || rules.rule[NO_SUN_COST]) return 0;
    return NativePrice(cards[card].seed);
}

static bool HasConveyor(Byte *board) {
    uintptr_t result = (uintptr_t)board;
    __asm__ volatile(
        "movl $0x41be50, %%edx\n\t"
        "call *%%edx"
        : "+a"(result) : : "ecx", "edx", "cc", "memory"
    );
    return (Byte)result != 0;
}

static bool CanOpenPicker(Byte *bank) {
    if (!bank || !InBattle() || !FIELD(Byte, bank, BANK_VISIBLE) ||
        FIELD(int, bank, BANK_PACKET_COUNT) < 1) return false;
    Byte *board = FIELD(Byte *, bank, BANK_BOARD);
    Byte *app = FIELD(Byte *, board, BOARD_APP);
    return FIELD(Byte *, app, APP_BOARD) == board &&
           (unsigned)FIELD(int, app, APP_MODE) <= 15 && !HasConveyor(board);
}

static Byte *ActiveChooser(void) {
    Byte *app = *(Byte **)0x6a9ec0;
    return app && FIELD(int, app, APP_SCENE) == SCENE_INTRO
        ? FIELD(Byte *, app, APP_CHOOSER) : NULL;
}

bool CanChangePlantUnlock(bool enabling) {
    Byte *chooser = ActiveChooser();
    return !chooser || (!FIELD(int, chooser, CHOOSER_IN_FLIGHT) &&
                        (enabling || !FIELD(int, chooser, CHOOSER_IN_BANK)));
}

void RefreshPlantPicker(void) {
    Byte *chooser = ActiveChooser();
    if (chooser) ((void (__attribute__((stdcall)) *)(Byte *))0x486f10)(chooser);
}

static bool CanSwitchChooser(Byte *chooser) {
    if (!chooser || chooser != ActiveChooser() || FIELD(int, chooser, CHOOSER_STATE) != 0)
        return false;
    Byte *app = FIELD(Byte *, chooser, CHOOSER_APP);
    Byte *board = FIELD(Byte *, chooser, CHOOSER_BOARD);
    if (!board || FIELD(Byte *, app, APP_BOARD) != board ||
        (unsigned)FIELD(int, app, APP_MODE) > 15 || HasConveyor(board)) return false;
    Byte *cutscene = FIELD(Byte *, board, BOARD_CUTSCENE);
    if (cutscene && !FIELD(Byte, cutscene, CUTSCENE_PRELOADED)) return false;

    typedef void *(__attribute__((thiscall)) *GetDialog)(Byte *, int);
    void **methods = *(void ***)app;
    GetDialog dialog = (GetDialog)methods[0x124 / 4];
    return !dialog(app, 3) && !dialog(app, 4);
}

static void ReturnHeldSeed(Byte *board) {
    __asm__ volatile(
        "movl $0x40cd80, %%ecx\n\t"
        "call *%%ecx"
        : "+a"(board) : : "ecx", "edx", "cc", "memory"
    );
}

static int GridRow(Byte *board, int x, int y) {
    __asm__ volatile(
        "pushl %%esi\n\t"
        "movl $0x41c550, %%esi\n\t"
        "call *%%esi\n\t"
        "popl %%esi"
        : "+a"(x), "+c"(y), "+d"(board) : : "cc", "memory"
    );
    return x;
}

NATIVE_NAKED static void FillNative(Byte *g UNUSED, int x UNUSED, int y UNUSED,
                                    int w UNUSED, int h UNUSED) {
    // The game's FillRect passes Graphics* in EAX and pops four stack arguments.
    __asm__(
        "movl 4(%esp), %eax\n\t"
        "pushl 20(%esp)\n\t"
        "pushl 20(%esp)\n\t"
        "pushl 20(%esp)\n\t"
        "pushl 20(%esp)\n\t"
        "movl $0x586d50, %ecx\n\t"
        "call *%ecx\n\t"
        "ret"
    );
}

static void Fill(Byte *g, int x, int y, int w, int h, Color color) {
    Color previous = FIELD(Color, g, GRAPHICS_COLOR);
    FIELD(Color, g, GRAPHICS_COLOR) = color;
    FillNative(g, x, y, w, h);
    FIELD(Color, g, GRAPHICS_COLOR) = previous;
}

typedef int (__attribute__((thiscall)) *StringWidth)(void *, const GameString *);
typedef void (__attribute__((thiscall)) *FontText)(void *, Byte *, int, int,
                                                  const GameString *, const Color *, const void *);
typedef void (__attribute__((stdcall)) *DrawBank)(Byte *, Byte *);
typedef void (__attribute__((stdcall)) *HitTest)(Byte *, int, int, Hit *);
typedef void (__attribute__((thiscall)) *DrawChooser)(Byte *, Byte *);

static void Label(Byte *g, const char *text, int center, int baseline, Color color) {
    void *font = *(void **)0x6a7384;
    if (!font) return;
    GameString label;
    label.reserved = 0;
    label.size = 0;
    label.capacity = 15;
    while (label.size < label.capacity && text[label.size]) {
        label.chars[label.size] = text[label.size];
        label.size++;
    }
    label.chars[label.size] = '\0';
    void **methods = *(void ***)font;
    int width = ((StringWidth)methods[0x1c / 4])(font, &label);
    ((FontText)methods[0x28 / 4])(font, g, center - width / 2, baseline,
                                 &label, &color, g + GRAPHICS_CLIP);
}

NATIVE_NAKED static void DrawNativeCard(Byte *g UNUSED, float x UNUSED, float y UNUSED,
                                        int seed UNUSED, int gray UNUSED, int draw_price UNUSED) {
    __asm__(
        "movl 20(%esp), %ecx\n\t"
        "pushl $0\n\t"
        "pushl 28(%esp)\n\t"
        "pushl $0\n\t"
        "pushl $-1\n\t"
        "pushl 32(%esp)\n\t"
        "pushl 32(%esp)\n\t"
        "pushl 32(%esp)\n\t"
        "pushl 32(%esp)\n\t"
        "movl $0x4876f0, %eax\n\t"
        "call *%eax\n\t"
        "addl $32, %esp\n\t"
        "ret"
    );
}

static void DrawAllyCard(Byte *g, int i, int x, int y, int gray, bool highlight) {
    int price = Price(i);
    DrawNativeCard(g, (float)x, (float)y, cards[i].seed, gray, price != 0);
    if (!price) Label(g, "0", x + 24, y + 65, off);
    if (highlight) {
        Fill(g, x - 2, y - 2, CARD_W + 4, 2, selected);
        Fill(g, x - 2, y + CARD_H, CARD_W + 4, 2, selected);
        Fill(g, x - 2, y, 2, CARD_H, selected);
        Fill(g, x + CARD_W, y, 2, CARD_H, selected);
    }
}

static void DrawSwitch(Byte *g, int x, int y, bool open) {
    Fill(g, x, y, SWITCH_W, SWITCH_H, rim);
    Fill(g, x + 1, y + 1, SWITCH_W - 2, SWITCH_H - 2, open ? active : off);
    Label(g, open ? "Plants" : "Zombies", x + SWITCH_W / 2, y + 17, ink);
}

static bool Equipped(int i) {
    return (equipped & (1u << i)) != 0;
}

void PvzDebugDrawBank(Byte *bank, Byte *g) {
    DrawBank native_draw = (DrawBank)0x489630;
    if (!CanOpenPicker(bank)) {
        native_draw(bank, g);
        return;
    }

    if (zombies_open) {
        // The draw-only bank header leaves the plant cards, cursor and cooldowns in their original storage.
        static uint32_t background[0x354 / 4];
        Byte *empty_bank = (Byte *)background;
        for (int offset = 0; offset < BANK_PACKET_START; offset += 4)
            FIELD(uint32_t, empty_bank, offset) = FIELD(uint32_t, bank, offset);
        FIELD(int, empty_bank, BANK_PACKET_COUNT) = 0;
        native_draw(empty_bank, g);

        int slot = 0;
        for (int i = 0; i < COUNT(cards); i++) {
            if (!Equipped(i)) continue;
            DrawAllyCard(g, i, CARD_X + slot++ * CARD_STEP, CARD_Y,
                         Price(i) <= Sun() ? 255 : 150, i == held_card);
        }

        const char *message = !equipped ? "No ally cards" : status ? status :
                              held_card < 0 ? "Pick a card" : cards[held_card].name;
        Fill(g, CARD_X, 88, 127, 21, active);
        Label(g, message, CARD_X + 63, 103, ink);
        if (held_card >= 0 && !status) {
            Fill(g, CARD_X + 129, 88, 91, 21, active);
            Label(g, "Pick a tile", CARD_X + 174, 103, ink);
        }
    } else {
        native_draw(bank, g);
    }

    int x = SWITCH_X - FIELD(int, bank, BANK_X);
    int y = SWITCH_Y - FIELD(int, bank, BANK_Y);
    DrawSwitch(g, x, y, zombies_open);
}

void PvzDebugDrawChooser(Byte *chooser, Byte *g) {
    ((DrawChooser)0x484690)(chooser, g);
    if (!CanSwitchChooser(chooser)) return;

    DrawSwitch(g, SWITCH_X - FIELD(int, chooser, CHOOSER_X),
               SWITCH_Y - FIELD(int, chooser, CHOOSER_Y), chooser_zombies_open);
    if (!chooser_zombies_open) return;

    int center = PICKER_X + PICKER_W / 2;
    Fill(g, PICKER_X, PICKER_Y, PICKER_W, PICKER_H, wood);
    Fill(g, PICKER_X + 2, 118, PICKER_W - 4, PICKER_H - 30, wood_inset);
    Label(g, "Choose zombies", center, 110, ink);
    int count = 0;
    for (int i = 0; i < COUNT(cards); i++) {
        bool on = Equipped(i);
        DrawAllyCard(g, i, PICKER_CARD_X + i * PICKER_CARD_STEP, PICKER_CARD_Y,
                     on ? 255 : 150, on);
        count += on;
    }
    char label[] = "0/6 equipped";
    label[0] += count;
    Label(g, label, center, 229, ink);
    Label(g, "Click to toggle", center, 250, ink);
    Label(g, "Sun on deploy", center, 271, ink);
}

int PvzDebugHidePlantHits(Byte *bank, Hit *result) {
    if (!zombies_open || !CanOpenPicker(bank)) return 0;
    result->object = NULL;
    result->type = 0;
    return 1;
}

static bool InRect(int x, int y, int left, int top, int w, int h) {
    return x >= left && x < left + w && y >= top && y < top + h;
}

int PvzDebugChooserClick(Byte *chooser, int x, int y, int clicks) {
    if (clicks <= 0 || panel_open || !CanSwitchChooser(chooser)) return 0;
    if (InRect(x + FIELD(int, chooser, CHOOSER_X), y + FIELD(int, chooser, CHOOSER_Y),
               SWITCH_X, SWITCH_Y, SWITCH_W, SWITCH_H)) {
        chooser_zombies_open = !chooser_zombies_open;
        return 1;
    }
    if (!chooser_zombies_open) return 0;
    for (int i = 0; i < COUNT(cards); i++) {
        if (InRect(x, y, PICKER_CARD_X + i * PICKER_CARD_STEP, PICKER_CARD_Y, CARD_W, CARD_H)) {
            equipped ^= 1u << i;
            return 1;
        }
    }
    return InRect(x, y, PICKER_X, PICKER_Y, PICKER_W, PICKER_H);
}

static bool TileAt(Byte *board, int x, int y, int *row, int *column) {
    if (x < 40 || x >= 760) return false;
    int col = (x - 40) / 80;
    int background = FIELD(int, board, BOARD_BACKGROUND);
    bool roof = background == 4 || background == 5;
    bool pool = background == 2 || background == 3;
    int top = 80 + (roof && col < 4 ? (4 - col) * 20 : 0);
    int rows = pool ? 6 : 5;
    int height = roof || pool ? 85 : 100;
    if (y < top || y >= top + rows * height) return false;
    *row = GridRow(board, x, y);
    *column = col;
    return *row >= 0 && *row < rows;
}

static int ConsumeMouse(Byte *board) {
    FIELD(Byte, board, BOARD_IGNORE_MOUSE_UP) = 1;
    return 1;
}

int PvzDebugPickerClick(Byte *board, int x, int y, int clicks) {
    Byte *bank = FIELD(Byte *, board, BOARD_BANK);
    if (!CanOpenPicker(bank) || FIELD(Byte, board, BOARD_PAUSED) ||
        FIELD(int, board, BOARD_TIME_STOP) > 0 || panel_open) return 0;

    if (clicks > 0 && InRect(x, y, SWITCH_X, SWITCH_Y, SWITCH_W, SWITCH_H)) {
        ReturnHeldSeed(board);
        zombies_open = !zombies_open;
        held_card = -1;
        status = NULL;
        return ConsumeMouse(board);
    }
    if (!zombies_open) return 0;
    if (clicks < 0)
        return CancelZombieSelection() ? ConsumeMouse(board) : 0;
    if (clicks == 0) return 0;

    int bank_x = x - FIELD(int, bank, BANK_X);
    int bank_y = y - FIELD(int, bank, BANK_Y);
    int slot = 0;
    for (int i = 0; i < COUNT(cards); i++) {
        if (!Equipped(i) ||
            !InRect(bank_x, bank_y, CARD_X + slot++ * CARD_STEP, CARD_Y, CARD_W, CARD_H)) continue;
        ReturnHeldSeed(board);
        held_card = held_card == i ? -1 : i;
        status = NULL;
        if (held_card >= 0 && Price(held_card) > Sun()) {
            held_card = -1;
            status = "Need more sun";
            FIELD(int, board, BOARD_LOW_SUN_FLASH) = 70;
        }
        return ConsumeMouse(board);
    }

    int row, column;
    if (held_card < 0 || !TileAt(board, x, y, &row, &column)) return 0;
    Hit hit;
    ((HitTest)0x411730)(board, x, y, &hit);
    if (hit.type == 3) return 0; // The normal click collects sun and coins first.
    int cost = Price(held_card);
    if (cost > Sun()) {
        status = "Need more sun";
        FIELD(int, board, BOARD_LOW_SUN_FLASH) = 70;
    } else if (!CanSpawn(cards[held_card].choice, row)) {
        status = "Pick a dry lane";
    } else if (SpawnZombie(cards[held_card].choice, row, column, true)) {
        if (cost) AddSun(-cost);
        held_card = -1;
        status = "Ally placed";
    } else {
        status = "Board is full";
    }
    return ConsumeMouse(board);
}

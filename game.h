#ifndef PVZ_DEBUG_GAME_H
#define PVZ_DEBUG_GAME_H

#include <stdbool.h>

enum Rule {
    FREE_PLANTING, NO_SUN_COST, INSTANT_RECHARGE, INSTANT_CHOMPER_SWALLOW,
    PLANT_PROTECTION, UNLOCK_ALL_PLANTS, RULE_COUNT
};

typedef struct {
    bool rule[RULE_COUNT];
    int speed;
    int display;
    int plant_health;
    int zombie_health;
} Rules;

enum { DEFAULT_HEALTH_STEP = 2 };

typedef struct {
    const wchar_t *name;
    int type;
    bool aquatic;
} ZombieChoice;

extern Rules rules;
extern bool panel_open;
extern bool remember_rules;
extern const ZombieChoice zombies[];
extern const int zombie_choice_count;
extern const wchar_t *const speed_names[];
extern const wchar_t *const display_names[];
extern const int health_percents[];

bool HasBoard(void);
bool InBattle(void);
bool HasWallet(void);
bool CanSpawn(int choice, int row);
bool HasNextWave(void);
int Sun(void);
int Coins(void);
bool SetRule(enum Rule rule, bool enabled);
void ApplyRules(void);
void AddSun(int amount);
void SetSun(int amount);
int AddCoins(int dollars);
int SetHealth(bool plants, int percent);
bool SpawnZombie(int choice, int row, int column, bool ally);
void AdvanceWave(void);
void ResetZombiePicker(void);
bool CancelZombieSelection(void);
bool CanChangePlantUnlock(bool enabling);
void RefreshPlantPicker(void);

#endif

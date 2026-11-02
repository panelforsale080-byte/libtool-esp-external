#pragma once
/*
 * Exact field table from working LibTool EspTab.cpp + esp_resolve.txt.
 * runtime == dump.cs for every ESP field.
 *
 * EspTab resolve path (same-process):
 *   Il2cpp::FindClass(name)
 *   klass->getField(name)->getOffset()
 *   klass->getField("Instance")->getStaticValue()
 *
 * External APK cannot call il2cpp_class_from_name. FindClass is reconstructed
 * by locating the class name string, then the Il2CppClass whose name ptr at
 * +0x10 matches, then static_fields + Instance offset.
 */
#include <cstdint>

namespace LibTool {

    /* BattleManager — dump.cs:684125 / EspTab DumpFallback */
    constexpr uintptr_t LocalPlayerShow = 0x48;   /* m_LocalPlayerShow */
    constexpr uintptr_t DicPlayerShow   = 0x60;   /* m_dicPlayerShow */
    constexpr uintptr_t DicMonsterShow  = 0x68;   /* m_dicMonsterShow */
    constexpr uintptr_t InstanceStatic  = 0x0;    /* static BattleManager Instance */

    /* ShowEntity — dump.cs:667695 */
    constexpr uintptr_t LastPosition = 0x3F4;     /* <LastPosition>k__BackingField */
    constexpr uintptr_t EntityId     = 0x18C;     /* m_ID */
    constexpr uintptr_t EntityDeath  = 0xC5;      /* m_bDeath */
    constexpr uintptr_t SameCamp     = 0x2A9;     /* m_bSameCampType */
    constexpr uintptr_t Hp           = 0x1A4;     /* m_Hp */
    constexpr uintptr_t HpMax        = 0x1A8;     /* m_HpMax */
    constexpr uintptr_t OwnSkillComp = 0x108;     /* m_OwnSkillComp */

    /* Battle.LogicFighter / LogicSkillComp / CoolDownComp */
    constexpr uintptr_t SkillComp    = 0x4F0;
    constexpr uintptr_t CoolDownComp = 0xB0;
    constexpr uintptr_t DicCoolInfo  = 0x18;
    constexpr uintptr_t SkillList    = 0x10;
    constexpr uintptr_t SkillUseType = 0x18;
    constexpr uintptr_t TranId       = 0x88;
    constexpr uintptr_t SummonSkillId = 0x984;

    /* BattleData / LogicBattleManager statics */
    constexpr uintptr_t BattleBridgeStatic = 0x0;  /* m_BattleBridge */
    constexpr uintptr_t LbmInstanceStatic  = 0x10; /* dump.cs:621136 Instance */

    /* Il2CppClass (Unity 2019–2021 arm64, same layout LibTool uses) */
    constexpr uintptr_t KlassImage = 0x00;
    constexpr uintptr_t KlassName  = 0x10;
    constexpr uintptr_t KlassNs    = 0x18;
    constexpr uintptr_t KlassParent = 0x58;
    constexpr uintptr_t StaticFieldOffs[] = {0xB8, 0xC0, 0xB0, 0xC8, 0xD0, 0xA8, 0xD8};

    /* EspTab::readIntPtrDict */
    constexpr uintptr_t DictEntries = 0x18;
    constexpr uintptr_t DictCount   = 0x20;
    constexpr uintptr_t ArrMaxLen   = 0x18;
    constexpr uintptr_t ArrData     = 0x20;
    constexpr uintptr_t EntryStride = 24;
    constexpr uintptr_t EntryHash   = 0x00;
    constexpr uintptr_t EntryKey    = 0x08;
    constexpr uintptr_t EntryValue  = 0x10;

    inline const char *kClasses[] = {
        "UnityEngine.Camera",
        "BattleData",
        "BattleBridge",
        "BattleManager",
        "ShowEntity",
        "ShowPlayer",
        "LogicBattleManager",
        "Battle.LogicFighter",
        "Battle.LogicSkillComp",
        "Battle.CoolDownComp",
        "Battle.CoolDownData",
        "Battle.ShowOwnSkillComp",
        "ShowSkillData",
        "ShowBattleCamera",
        "GameMethod",
    };

    inline const int kMonsterIds[] = {
        2002, 2003, 2004, 2005, 2006, 2008, 2009, 2011, 2012, 2013, 2056, 2059, 2072
    };

    inline const char *monsterName(int id) {
        switch (id) {
        case 2002: return "Lord";
        case 2003: return "Turtle";
        case 2004: return "Fiend";
        case 2005: return "Serpent";
        case 2006: return "Scaled Lizard";
        case 2008:
        case 2059: return "Crammer";
        case 2009: return "Rockursa";
        case 2011:
        case 2013: return "Crab";
        case 2012: return "Serpent Kids";
        case 2056:
        case 2072: return "Lithowanderer";
        default:   return "Monster";
        }
    }
}

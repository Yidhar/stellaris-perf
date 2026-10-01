// SUBSET of the generated Stellaris SDK header, written by tools/extract_sdk.py. DO NOT EDIT.
// The full header is produced from the installed stellaris.exe by tools/sdk_dumper (Stellaris MCP
// repository); this file keeps only what perf/ and bench/ use. Values are RVAs / offsets for the
// stellaris.exe whose PE TimeDateStamp is sdk::kExeTimestamp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sdk {
inline constexpr uint32_t kExeTimestamp = 0x6AB5181D;  // PE TimeDateStamp this SDK was dumped from

namespace glob {
    inline constexpr uintptr_t CRandom_Forbidden = 0x2812CF9;  // CRandom_Forbidden  score=anchor live=anchor
    inline constexpr uintptr_t UpdateShipParallel_GrainClamp = 0x2525BA;  // UpdateShipParallel_GrainClamp  score=anchor live=anchor
    inline constexpr uintptr_t g_CurrentGameState = 0x3113A08;  // g_CurrentGameState  score=0.768 live=not-checked
    inline constexpr uintptr_t g_CurrentInGameIdler = 0x3114180;  // g_CurrentInGameIdler  score=0.773 live=not-checked
    inline constexpr uintptr_t g_bFrameSmoothing = 0x2806830;  // g_bFrameSmoothing  score=anchor live=anchor
}  // namespace glob

namespace rt {
    inline constexpr std::ptrdiff_t CEventScope_event_targets = 0x48;
    inline constexpr std::ptrdiff_t CEventScope_from = 0x38;
    inline constexpr std::ptrdiff_t CEventTarget_is_event_target = 0x198;
    inline constexpr std::ptrdiff_t CFleetManagerView_reinforce_due = 0x32B8;
    inline constexpr std::ptrdiff_t CGameIdler_is_multiplayer = 0x180;
    inline constexpr std::ptrdiff_t CGameState_date_hours = 0xC0;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_dynamic_size = 0x248;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_flag = 0x220;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_vt_GetFlags = 0xF0;
    inline constexpr std::ptrdiff_t CInGameIdler_paused = 0x594;
    inline constexpr std::ptrdiff_t CInGameIdler_speed = 0x590;
    inline constexpr std::ptrdiff_t CModifierNodeManager_batch = 0x989;
    inline constexpr std::ptrdiff_t CModifierNodeManager_busy = 0x940;
    inline constexpr std::ptrdiff_t CModifierNodeManager_has_invalid = 0x941;
    inline constexpr std::ptrdiff_t CModifierNodeManager_masked = 0x988;
    inline constexpr std::ptrdiff_t CModifierNodeManager_slot_count = 0x14;
    inline constexpr std::ptrdiff_t CModifierNodeManager_slot_node = 0x8;
    inline constexpr std::ptrdiff_t CModifierNodeManager_slots = 0x8;
    inline constexpr std::ptrdiff_t CModifierNode_dirty = 0x108;
    inline constexpr std::ptrdiff_t CModifierNode_modifier = 0x50;
    inline constexpr std::ptrdiff_t CModifier_entries = 0x38;
    inline constexpr std::ptrdiff_t CModifier_entry_count = 0x44;
    inline constexpr std::ptrdiff_t CModifier_parent_count = 0x9C;
    inline constexpr std::ptrdiff_t CModifier_parents = 0x90;
    inline constexpr std::ptrdiff_t CPdxIntegerFlags_count = 0x1C;
    inline constexpr std::ptrdiff_t CPdxIntegerFlags_days = 0x40;
    inline constexpr std::ptrdiff_t CPdxIntegerFlags_ids = 0x10;
    inline constexpr std::ptrdiff_t CRandomLog_config = 0x38;
}  // namespace rt

namespace fn {
    inline constexpr uintptr_t CCountry_CalcOurOpinionOfOther = 0x705C90;
    inline constexpr uintptr_t CEventScope_Copy = 0x393EE0;
    inline constexpr uintptr_t CEventTarget_GetScope = 0x352950;
    inline constexpr uintptr_t CFleetManagerTemplateGridController_Update = 0x149D5C0;
    inline constexpr uintptr_t CFleetManagerView_Update = 0x112A050;
    inline constexpr uintptr_t CFleet_CalcMilitaryPower = 0xC7EFF0;
    inline constexpr uintptr_t CGameState_HandleTurnTick = 0x2511D0;
    inline constexpr uintptr_t CGameState_OnNewGameStarted = 0x270AC0;
    inline constexpr uintptr_t CGameState_OnSavedGameStarted = 0x271340;
    inline constexpr uintptr_t CHasFlagTrigger_ActualEvaluate = 0x195A140;
    inline constexpr uintptr_t CInGameIdler_SetGameSpeed = 0x935BE0;
    inline constexpr uintptr_t CInGameIdler_SetPaused = 0x9362D0;
    inline constexpr uintptr_t CModifierNodeBase_Update = 0x237C90;
    inline constexpr uintptr_t CModifierNodeManager_AddInvalid = 0x239050;
    inline constexpr uintptr_t CModifierNodeManager_Update = 0x29D3C0;
    inline constexpr uintptr_t CPdxIntegerFlags_UpdateFlags = 0x1A7D250;
    inline constexpr uintptr_t CRandomLog_Get = 0x1B47DE0;
    inline constexpr uintptr_t CScriptedRule_Evaluate = 0x5B71D0;
    inline constexpr uintptr_t GetDynamicFlag = 0x9F8A10;
}  // namespace fn

}  // namespace sdk

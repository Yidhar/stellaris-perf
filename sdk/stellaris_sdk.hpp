// SUBSET of the generated Stellaris SDK header, written by tools/extract_sdk.py. DO NOT EDIT.
// The full header is produced from the installed stellaris.exe by tools/sdk_dumper (Stellaris MCP
// repository); this file keeps only what perf/ and bench/ use. Values are RVAs / offsets for the
// stellaris.exe whose PE TimeDateStamp is sdk::kExeTimestamp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sdk {
inline constexpr uint32_t kExeTimestamp = 0x6AE49C1B;  // PE TimeDateStamp this SDK was dumped from
inline constexpr uintptr_t kRvaEngineAlloc = 0x2026338;

namespace glob {
    inline constexpr uintptr_t CModifier_Definitions = 0x2819390;  // CModifier::_Definitions  score=anchor live=anchor
    inline constexpr uintptr_t CRandom_Forbidden = 0x2818CFD;  // CRandom_Forbidden  score=anchor live=anchor
    inline constexpr uintptr_t UpdateShipParallel_GrainClamp = 0x252CAA;  // UpdateShipParallel_GrainClamp  score=anchor live=anchor
    inline constexpr uintptr_t g_CurrentGameState = 0x3119740;  // g_CurrentGameState  score=0.781 live=not-checked
    inline constexpr uintptr_t g_CurrentInGameIdler = 0x3119EB0;  // g_CurrentInGameIdler  score=0.81 live=not-checked
    inline constexpr uintptr_t g_bFrameSmoothing = 0x280C830;  // g_bFrameSmoothing  score=anchor live=anchor
}  // namespace glob

namespace rt {
    inline constexpr std::ptrdiff_t CEventScope_event_targets = 0x48;
    inline constexpr std::ptrdiff_t CEventScope_from = 0x38;
    inline constexpr std::ptrdiff_t CEventTarget_is_event_target = 0x198;
    inline constexpr std::ptrdiff_t CFleetManagerView_reinforce_due = 0x32B8;
    inline constexpr std::ptrdiff_t CGameState_date_hours = 0xC0;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_dynamic_size = 0x248;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_flag = 0x220;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_vt_GetFlags = 0xF0;
    inline constexpr std::ptrdiff_t CInGameIdler_paused = 0x594;
    inline constexpr std::ptrdiff_t CInGameIdler_speed = 0x590;
    inline constexpr std::ptrdiff_t CModifierNodeManager_batch = 0x949;
    inline constexpr std::ptrdiff_t CModifierNodeManager_busy = 0x900;
    inline constexpr std::ptrdiff_t CModifierNodeManager_has_invalid = 0x901;
    inline constexpr std::ptrdiff_t CModifierNodeManager_masked = 0x948;
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
    inline constexpr uintptr_t Application_Update = 0x1A520F0;
    inline constexpr uintptr_t CColony_CalculateModifier = 0xDB2BD0;
    inline constexpr uintptr_t CColony_InvalidateModifier = 0xDB2280;
    inline constexpr uintptr_t CColony_MicroUpdateCachedValues = 0xDDBE80;
    inline constexpr uintptr_t CColony_MonthlyUpdateSerial = 0xDC90D0;
    inline constexpr uintptr_t CCountry_CalcOurOpinionOfOther = 0x7065D0;
    inline constexpr uintptr_t CEventScope_Copy = 0x394F20;
    inline constexpr uintptr_t CEventTarget_GetScope = 0x3539D0;
    inline constexpr uintptr_t CFile_WriteCStr = 0x1B4EA50;
    inline constexpr uintptr_t CFleetManagerTemplateGridController_Update = 0x14A4870;
    inline constexpr uintptr_t CFleetManagerView_Update = 0x11308C0;
    inline constexpr uintptr_t CFleet_CalcMilitaryPower = 0xC84A00;
    inline constexpr uintptr_t CGameIdler_DrawStep = 0x334B50;
    inline constexpr uintptr_t CGameIdler_Idle = 0x3338E0;
    inline constexpr uintptr_t CGameState_DailyUpdate = 0x25C3C0;
    inline constexpr uintptr_t CGameState_HandleTurnTick = 0x2518C0;
    inline constexpr uintptr_t CGameState_MicroUpdate = 0x252D80;
    inline constexpr uintptr_t CGameState_MonthlyUpdate = 0x260BF0;
    inline constexpr uintptr_t CGameState_ProcessAI = 0x263C10;
    inline constexpr uintptr_t CGameState_YearlyUpdate = 0x2624B0;
    inline constexpr uintptr_t CHasFlagTrigger_ActualEvaluate = 0x195F2B0;
    inline constexpr uintptr_t CInGameIdler_SaveGame = 0x935ED0;
    inline constexpr uintptr_t CInGameIdler_SetGameSpeed = 0x9368E0;
    inline constexpr uintptr_t CInGameIdler_SetPaused = 0x936FD0;
    inline constexpr uintptr_t CInGameIdler_Update = 0x92B030;
    inline constexpr uintptr_t CMeanTimeToHappen_GetRawFactor = 0x342F30;
    inline constexpr uintptr_t CMemoryFile_GetSize = 0x1B4E9D0;
    inline constexpr uintptr_t CMemoryFile_WriteData = 0x1B4EAB0;
    inline constexpr uintptr_t CModifierNodeBase_Clear = 0x30F390;
    inline constexpr uintptr_t CModifierNodeBase_InvalidateChildren = 0x238340;
    inline constexpr uintptr_t CModifierNodeBase_Update = 0x238210;
    inline constexpr uintptr_t CModifierNodeManager_AddInvalid = 0x2395D0;
    inline constexpr uintptr_t CModifierNodeManager_Update = 0x29E3A0;
    inline constexpr uintptr_t CModifier_ClearEntries = 0x36E740;
    inline constexpr uintptr_t CPdxIntegerFlags_UpdateFlags = 0x1A824B0;
    inline constexpr uintptr_t CPdxParallelForDescriptor_Ctor = 0x1B54BD0;
    inline constexpr uintptr_t CPlanet_CalculateModifier = 0xE099A0;
    inline constexpr uintptr_t CPopFaction_UpdateSupportAndApproval = 0x69ABF0;
    inline constexpr uintptr_t CRT_operator_delete = 0x2026374;
    inline constexpr uintptr_t CRT_snprintf = 0x1B671F0;
    inline constexpr uintptr_t CRT_sprintf = 0xD3DE30;
    inline constexpr uintptr_t CRT_stdio_common_vsprintf = 0x2039EA4;
    inline constexpr uintptr_t CRT_stdio_printf_options = 0x15B590;
    inline constexpr uintptr_t CRandomLog_Get = 0x1B4CD60;
    inline constexpr uintptr_t CSaveGameTaskManager_Update = 0xA8EAE0;
    inline constexpr uintptr_t CScriptedRule_Evaluate = 0x5B8420;
    inline constexpr uintptr_t CStaticLexer_RebuildTable = 0x1BBA6A0;
    inline constexpr uintptr_t CStaticLexer_Table = 0x1BBA630;
    inline constexpr uintptr_t CWriter_WriteCString = 0x1BBA370;
    inline constexpr uintptr_t CWriter_WriteIndent = 0x1BB9080;
    inline constexpr uintptr_t CWriter_WriteString = 0x1BB8B60;
    inline constexpr uintptr_t CWriter_WriteTokenText = 0x16A980;
    inline constexpr uintptr_t GetDynamicFlag = 0x9F9640;
    inline constexpr uintptr_t Gui_Update = 0x1C0F330;
    inline constexpr uintptr_t NColonyUpdate_MonthlyUpdate = 0xE45DB0;
    inline constexpr uintptr_t NPlanetJobs_UpdatePopJobs = 0xB2AE00;
    inline constexpr uintptr_t UpdateAIParallel = 0xB8C6B0;
}  // namespace fn

namespace site {
    inline constexpr uintptr_t CColony_AddSizeToPopGroup_NodeUpdate = 0xDB5FC4;
    inline constexpr uintptr_t CColony_MoveSizeToPopGroup_NodeUpdate_0 = 0xDB6080;
    inline constexpr uintptr_t CColony_MoveSizeToPopGroup_NodeUpdate_1 = 0xDB6089;
    inline constexpr uintptr_t CColony_OnAddPops_CachedValues = 0xDB4E72;
    inline constexpr uintptr_t CColony_OnAddPops_NodeUpdate = 0xDB4E63;
    inline constexpr uintptr_t CColony_OnRemovePops_CachedValues = 0xDB4EF4;
    inline constexpr uintptr_t CColony_OnRemovePops_NodeUpdate = 0xDB4EE5;
    inline constexpr uintptr_t CCountryRelayNetworkManager_UpdateRelayNetwork_InvalidateChildren = 0x753FC8;
    inline constexpr uintptr_t CCountry_InvalidateModifier_InvalidateChildren = 0x23EEE7;
    inline constexpr uintptr_t CCountry_InvalidateRelayNetworkModifier_InvalidateChildren = 0x5756C6;
    inline constexpr uintptr_t CModifierNodeBase_Invalidate_AddInvalid = 0x23969E;
    inline constexpr uintptr_t NColonyUpdate_DailyUpdatePopJobs_UpdatePopJobs_0 = 0xE45D6E;
    inline constexpr uintptr_t NColonyUpdate_DailyUpdatePopJobs_UpdatePopJobs_1 = 0xE45D96;
    inline constexpr uintptr_t NColonyUpdate_MonthlyUpdate_UpdatePopJobs_0 = 0xE45F99;
    inline constexpr uintptr_t NColonyUpdate_MonthlyUpdate_UpdatePopJobs_1 = 0xE45FBE;
    inline constexpr uintptr_t NPlanetJobs_AssignPopsToJobs_NodeUpdate_0 = 0xB3B4D1;
    inline constexpr uintptr_t NPlanetJobs_AssignPopsToJobs_NodeUpdate_1 = 0xB3B4DA;
    inline constexpr uintptr_t NPlanetJobs_UpdatePopJobs_ColonyNodeUpdate = 0xB2AEAD;
}  // namespace site

namespace ent {
namespace CFleet {
    inline constexpr std::ptrdiff_t ships = 0x320;  // tok 0x2c15 ref_array<unsigned_int>  [check: positional]
}
}  // namespace ent

}  // namespace sdk

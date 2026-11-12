// SUBSET of the generated Stellaris SDK header, written by tools/extract_sdk.py. DO NOT EDIT.
// The full header is produced from the installed stellaris.exe by tools/sdk_dumper (Stellaris MCP
// repository); this file keeps only what perf/ and bench/ use. Values are RVAs / offsets for the
// stellaris.exe whose PE TimeDateStamp is sdk::kExeTimestamp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sdk {
inline constexpr uint32_t kExeTimestamp = 0x6AF23393;  // PE TimeDateStamp this SDK was dumped from
inline constexpr uintptr_t kRvaEngineAlloc = 0x2028B18;

namespace glob {
    inline constexpr uintptr_t CModifier_Definitions = 0x281B390;  // CModifier::_Definitions  score=anchor live=anchor
    inline constexpr uintptr_t CRandom_Forbidden = 0x281AD04;  // CRandom_Forbidden  score=anchor live=anchor
    inline constexpr uintptr_t CScriptedRule_TokenByIndex = 0x2839870;  // CScriptedRule_TokenByIndex  score=anchor live=anchor
    inline constexpr uintptr_t CStaticLexer_Instance = 0x35F9460;  // CStaticLexer_Instance  score=anchor live=anchor
    inline constexpr uintptr_t UpdateShipParallel_GrainClamp = 0x252B6A;  // UpdateShipParallel_GrainClamp  score=anchor live=anchor
    inline constexpr uintptr_t UseSavedRandom = 0x311BEE0;  // UseSavedRandom  score=anchor live=anchor
    inline constexpr uintptr_t g_CurrentGameState = 0x311B708;  // g_CurrentGameState  score=0.771 live=not-checked
    inline constexpr uintptr_t g_CurrentInGameIdler = 0x311BF10;  // g_CurrentInGameIdler  score=0.791 live=not-checked
    inline constexpr uintptr_t g_bFrameSmoothing = 0x280E838;  // g_bFrameSmoothing  score=anchor live=anchor
}  // namespace glob

namespace rt {
    inline constexpr std::ptrdiff_t CBlob_capacity = 0x10;
    inline constexpr std::ptrdiff_t CBlob_data = 0x8;
    inline constexpr std::ptrdiff_t CBlob_size = 0x14;
    inline constexpr std::ptrdiff_t CColony_flags = 0x1088;
    inline constexpr std::ptrdiff_t CColony_flags_clear_node_bit = 0x2;
    inline constexpr std::ptrdiff_t CColony_modifier = 0x170;
    inline constexpr std::ptrdiff_t CColony_modifier_node = 0x120;
    inline constexpr std::ptrdiff_t CEventScope_event_targets = 0x48;
    inline constexpr std::ptrdiff_t CEventScope_from = 0x38;
    inline constexpr std::ptrdiff_t CEventScope_id = 0x10;
    inline constexpr std::ptrdiff_t CEventScope_local = 0x1C;
    inline constexpr std::ptrdiff_t CEventScope_param_count = 0x84;
    inline constexpr std::ptrdiff_t CEventScope_prev = 0x40;
    inline constexpr std::ptrdiff_t CEventScope_root = 0x30;
    inline constexpr std::ptrdiff_t CEventScope_type = 0x8;
    inline constexpr std::ptrdiff_t CEventTarget_is_event_target = 0x198;
    inline constexpr std::ptrdiff_t CFleetManagerView_reinforce_due = 0x32B8;
    inline constexpr std::ptrdiff_t CGameState_date_hours = 0xC0;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_dynamic_size = 0x248;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_flag = 0x220;
    inline constexpr std::ptrdiff_t CHasFlagTrigger_vt_GetFlags = 0xF0;
    inline constexpr std::ptrdiff_t CInGameIdler_paused = 0x594;
    inline constexpr std::ptrdiff_t CInGameIdler_speed = 0x590;
    inline constexpr std::ptrdiff_t CMemoryFile_blob = 0x80;
    inline constexpr std::ptrdiff_t CMemoryFile_pos = 0x94;
    inline constexpr std::ptrdiff_t CModifierDefinition_size = 0xB0;
    inline constexpr std::ptrdiff_t CModifierDefinition_token = 0x90;
    inline constexpr std::ptrdiff_t CModifierDefinitions_count = 0xC;
    inline constexpr std::ptrdiff_t CModifierEntry_type = 0x8;
    inline constexpr std::ptrdiff_t CModifierNodeManager_batch = 0x949;
    inline constexpr std::ptrdiff_t CModifierNodeManager_busy = 0x900;
    inline constexpr std::ptrdiff_t CModifierNodeManager_has_invalid = 0x901;
    inline constexpr std::ptrdiff_t CModifierNodeManager_index_mask = 0xFFFFF;
    inline constexpr std::ptrdiff_t CModifierNodeManager_masked = 0x948;
    inline constexpr std::ptrdiff_t CModifierNodeManager_slot_count = 0x14;
    inline constexpr std::ptrdiff_t CModifierNodeManager_slot_node = 0x8;
    inline constexpr std::ptrdiff_t CModifierNodeManager_slot_size = 0x18;
    inline constexpr std::ptrdiff_t CModifierNodeManager_slots = 0x8;
    inline constexpr std::ptrdiff_t CModifierNodeType_category = 0x8;
    inline constexpr std::ptrdiff_t CModifierNode_calc = 0x48;
    inline constexpr std::ptrdiff_t CModifierNode_dirty = 0x108;
    inline constexpr std::ptrdiff_t CModifierNode_id = 0x8;
    inline constexpr std::ptrdiff_t CModifierNode_modifier = 0x50;
    inline constexpr std::ptrdiff_t CModifierParent_category = 0x18;
    inline constexpr std::ptrdiff_t CModifierParent_multiplier = 0x8;
    inline constexpr std::ptrdiff_t CModifierParent_name = 0x10;
    inline constexpr std::ptrdiff_t CModifier_entries = 0x38;
    inline constexpr std::ptrdiff_t CModifier_entry_count = 0x44;
    inline constexpr std::ptrdiff_t CModifier_entry_size = 0x10;
    inline constexpr std::ptrdiff_t CModifier_parent_count = 0x9C;
    inline constexpr std::ptrdiff_t CModifier_parent_size = 0x20;
    inline constexpr std::ptrdiff_t CModifier_parents = 0x90;
    inline constexpr std::ptrdiff_t CPdxIntegerFlags_count = 0x1C;
    inline constexpr std::ptrdiff_t CPdxIntegerFlags_days = 0x40;
    inline constexpr std::ptrdiff_t CPdxIntegerFlags_ids = 0x10;
    inline constexpr std::ptrdiff_t CPdxParallelForDescriptor_tasks = 0x14;
    inline constexpr std::ptrdiff_t CPopFaction_approval = 0x50;
    inline constexpr std::ptrdiff_t CPopFaction_support = 0x40;
    inline constexpr std::ptrdiff_t CRandomLog_config = 0x38;
    inline constexpr std::ptrdiff_t CScriptedRule_index = 0x0;
    inline constexpr std::ptrdiff_t CStaticLexer_built = 0x7C;
    inline constexpr std::ptrdiff_t CStaticLexer_entries = 0x70;
    inline constexpr std::ptrdiff_t CStaticLexer_entry_size = 0x30;
    inline constexpr std::ptrdiff_t CStaticLexer_wanted = 0x80;
    inline constexpr std::ptrdiff_t CString_text = 0x10;
    inline constexpr std::ptrdiff_t CWriter_binary = 0x18;
    inline constexpr std::ptrdiff_t CWriter_depth = 0x8;
    inline constexpr std::ptrdiff_t CWriter_file = 0x10;
    inline constexpr std::ptrdiff_t CWriter_token_eol = 0x10;
    inline constexpr std::ptrdiff_t CWriter_token_fixed_point = 0x167;
    inline constexpr std::ptrdiff_t CWriter_token_indent = 0x11;
}  // namespace rt

namespace vt {
    inline constexpr int CCountryAI_Update = 7;
    inline constexpr int CFile_GetSize = 16;
    inline constexpr int CFile_WriteCStr = 6;
    inline constexpr int CFile_WriteData = 9;
    inline constexpr int CModifierNodeCalc_Call = 2;
    inline constexpr int CPdxModifierName_CalcHash = 5;
}  // namespace vt

namespace fn {
    inline constexpr uintptr_t Application_Update = 0x1A53D60;
    inline constexpr uintptr_t CColony_CalculateModifier = 0xDB3D90;
    inline constexpr uintptr_t CColony_InvalidateModifier = 0xDB3440;
    inline constexpr uintptr_t CColony_MicroUpdateCachedValues = 0xDDD140;
    inline constexpr uintptr_t CColony_MonthlyUpdateSerial = 0xDCA380;
    inline constexpr uintptr_t CCountry_CalcOurOpinionOfOther = 0x706720;
    inline constexpr uintptr_t CEventScope_Copy = 0x398180;
    inline constexpr uintptr_t CEventTarget_GetScope = 0x356C30;
    inline constexpr uintptr_t CFile_WriteCStr = 0x1B50860;
    inline constexpr uintptr_t CFleetManagerTemplateGridController_Update = 0x14A6230;
    inline constexpr uintptr_t CFleetManagerView_Update = 0x11321C0;
    inline constexpr uintptr_t CFleet_CalcMilitaryPower = 0xC85DB0;
    inline constexpr uintptr_t CGameIdler_DrawStep = 0x337DB0;
    inline constexpr uintptr_t CGameIdler_Idle = 0x336B40;
    inline constexpr uintptr_t CGameState_DailyUpdate = 0x25C850;
    inline constexpr uintptr_t CGameState_HandleTurnTick = 0x251780;
    inline constexpr uintptr_t CGameState_MicroUpdate = 0x252C40;
    inline constexpr uintptr_t CGameState_MonthlyUpdate = 0x261090;
    inline constexpr uintptr_t CGameState_ProcessAI = 0x2640B0;
    inline constexpr uintptr_t CGameState_YearlyUpdate = 0x262950;
    inline constexpr uintptr_t CHasFlagTrigger_ActualEvaluate = 0x1960EE0;
    inline constexpr uintptr_t CInGameIdler_SaveGame = 0x937820;
    inline constexpr uintptr_t CInGameIdler_SetGameSpeed = 0x938230;
    inline constexpr uintptr_t CInGameIdler_SetPaused = 0x938920;
    inline constexpr uintptr_t CInGameIdler_Update = 0x92C980;
    inline constexpr uintptr_t CMeanTimeToHappen_GetRawFactor = 0x346190;
    inline constexpr uintptr_t CMemoryFile_GetSize = 0x1B507E0;
    inline constexpr uintptr_t CMemoryFile_WriteData = 0x1B508C0;
    inline constexpr uintptr_t CModifierNodeBase_Clear = 0x312550;
    inline constexpr uintptr_t CModifierNodeBase_InvalidateChildren = 0x238300;
    inline constexpr uintptr_t CModifierNodeBase_Update = 0x2381D0;
    inline constexpr uintptr_t CModifierNodeManager_AddInvalid = 0x239590;
    inline constexpr uintptr_t CModifierNodeManager_Update = 0x29E860;
    inline constexpr uintptr_t CModifier_ClearEntries = 0x3719A0;
    inline constexpr uintptr_t CPdxIntegerFlags_UpdateFlags = 0x1A84120;
    inline constexpr uintptr_t CPdxParallelForDescriptor_Ctor = 0x1B569E0;
    inline constexpr uintptr_t CPlanet_CalculateModifier = 0xE0AC80;
    inline constexpr uintptr_t CPopFaction_UpdateSupportAndApproval = 0x69AD50;
    inline constexpr uintptr_t CRT_operator_delete = 0x2028B54;
    inline constexpr uintptr_t CRT_snprintf = 0x1B69000;
    inline constexpr uintptr_t CRT_sprintf = 0xD3F3B0;
    inline constexpr uintptr_t CRT_stdio_common_vsprintf = 0x203C684;
    inline constexpr uintptr_t CRT_stdio_printf_options = 0x15B570;
    inline constexpr uintptr_t CRandomLog_Get = 0x1B4EB70;
    inline constexpr uintptr_t CSaveGameTaskManager_Update = 0xA8F9B0;
    inline constexpr uintptr_t CScriptedRule_Evaluate = 0x5BB5A0;
    inline constexpr uintptr_t CStaticLexer_RebuildTable = 0x1BBC4B0;
    inline constexpr uintptr_t CStaticLexer_Table = 0x1BBC440;
    inline constexpr uintptr_t CWriter_WriteCString = 0x1BBC180;
    inline constexpr uintptr_t CWriter_WriteIndent = 0x1BBAE90;
    inline constexpr uintptr_t CWriter_WriteString = 0x1BBA970;
    inline constexpr uintptr_t CWriter_WriteTokenText = 0x16A960;
    inline constexpr uintptr_t GetDynamicFlag = 0x9FA5A0;
    inline constexpr uintptr_t Gui_Update = 0x1C11400;
    inline constexpr uintptr_t NColonyUpdate_MonthlyUpdate = 0xE47350;
    inline constexpr uintptr_t NPlanetJobs_UpdatePopJobs = 0xB2BFF0;
    inline constexpr uintptr_t UpdateAIParallel = 0xB8DB40;
}  // namespace fn

namespace site {
    inline constexpr uintptr_t CColony_AddSizeToPopGroup_NodeUpdate = 0xDB7184;
    inline constexpr uintptr_t CColony_MoveSizeToPopGroup_NodeUpdate_0 = 0xDB7240;
    inline constexpr uintptr_t CColony_MoveSizeToPopGroup_NodeUpdate_1 = 0xDB7249;
    inline constexpr uintptr_t CColony_OnAddPops_CachedValues = 0xDB6032;
    inline constexpr uintptr_t CColony_OnAddPops_NodeUpdate = 0xDB6023;
    inline constexpr uintptr_t CColony_OnRemovePops_CachedValues = 0xDB60B4;
    inline constexpr uintptr_t CColony_OnRemovePops_NodeUpdate = 0xDB60A5;
    inline constexpr uintptr_t CCountryRelayNetworkManager_UpdateRelayNetwork_InvalidateChildren = 0x754268;
    inline constexpr uintptr_t CCountry_InvalidateModifier_InvalidateChildren = 0x23EEA7;
    inline constexpr uintptr_t CCountry_InvalidateRelayNetworkModifier_InvalidateChildren = 0x578966;
    inline constexpr uintptr_t CModifierNodeBase_Invalidate_AddInvalid = 0x23965E;
    inline constexpr uintptr_t NColonyUpdate_DailyUpdatePopJobs_UpdatePopJobs_0 = 0xE4730E;
    inline constexpr uintptr_t NColonyUpdate_DailyUpdatePopJobs_UpdatePopJobs_1 = 0xE47336;
    inline constexpr uintptr_t NColonyUpdate_MonthlyUpdate_UpdatePopJobs_0 = 0xE47539;
    inline constexpr uintptr_t NColonyUpdate_MonthlyUpdate_UpdatePopJobs_1 = 0xE4755E;
    inline constexpr uintptr_t NPlanetJobs_AssignPopsToJobs_NodeUpdate_0 = 0xB3C861;
    inline constexpr uintptr_t NPlanetJobs_AssignPopsToJobs_NodeUpdate_1 = 0xB3C86A;
    inline constexpr uintptr_t NPlanetJobs_UpdatePopJobs_ColonyNodeUpdate = 0xB2C09D;
}  // namespace site

namespace ent {
namespace CFleet {
    inline constexpr std::ptrdiff_t ships = 0x320;  // tok 0x2c15 ref_array<unsigned_int>  [check: positional]
}
namespace CPlanet {
    inline constexpr std::ptrdiff_t colony = 0xE0;  // tok 0x3931 ref<CColony> via CColonyCarrier
}
}  // namespace ent

}  // namespace sdk

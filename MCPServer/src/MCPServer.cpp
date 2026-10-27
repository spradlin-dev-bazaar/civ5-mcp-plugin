#include "CvGameCoreDLLPCH.h"

#include "CivHooks.h"
#include "CivPluginApi.h"

#include "CvDealAI.h"
#include "CvDllPlayer.h"
#include "CvDllPreGame.h"
#include "McpBridge.h"
#include "McpDiplomacy.h"
#include "McpLua.h"
#include "McpMapDraw.h"
#include "McpSeat.h"
#include "McpVersion.h"

#include <stdarg.h>
#include <stdio.h>

#define PLUGIN_EXPORT extern "C" __declspec(dllexport)

CIV_PLUGIN_EXPORT_API_VERSION;

static const CivHostInfo* s_host = 0;

// Writes a line to Logs\Bazaar.log, headed with this plugin's name.
static void Log(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
	va_end(args);
	s_host->Log(s_host->pluginIndex, line);
}

static void Check(int result, const char* hook)
{
	if (result != CIV_HOOK_OK)
		Log("could not register %s (result %d)", hook, result);
}

static bool BeforeGameUpdate(CvGame* self)
{
	McpBridge::Get().Tick();
	return false;
}

static void AfterGameRead(CvGame* self, FDataStream& kStream)
{
	McpBridge::Get().NoteGameLoaded();
}

static void AfterDllPlayerIsHuman(const CvDllPlayer* self, bool& result)
{
	result = result && McpEngineSeesAsHuman(self->GetID());
}

static void AfterDllPreGameIsHuman(CvDllPreGame* self, PlayerTypes ePlayer, bool& result)
{
	result = result && McpEngineSeesAsHuman(ePlayer);
}

static bool BeforeSendRequest(PlayerTypes eFrom, PlayerTypes eTo, DiploUIStateTypes eState, DiploMessageTypes eMessage, const char* szMessage, LeaderheadAnimationTypes eAnimation, int iExtraGameData)
{
	return McpInterceptRequest(eFrom, eTo, eState, szMessage, iExtraGameData);
}

static bool BeforeSendDealRequest(PlayerTypes eFrom, PlayerTypes eTo, CvDeal* pDeal, DiploUIStateTypes eState, DiploMessageTypes eMessage, const char* szMessage, LeaderheadAnimationTypes eAnimation)
{
	return McpInterceptDealRequest(eFrom, eTo, pDeal, eState, szMessage);
}

static bool BeforeEqualizeDeal(CvDealAI* self, CvDeal* pDeal, PlayerTypes eOtherPlayer, bool bDontChangeMyExistingItems, bool bDontChangeTheirExistingItems, bool& bDealGoodToBeginWith, bool& bCantMatchOffer, bool& result)
{
	McpBeginEqualizing(eOtherPlayer);
	return false;
}

static void AfterEqualizeDeal(CvDealAI* self, CvDeal* pDeal, PlayerTypes eOtherPlayer, bool bDontChangeMyExistingItems, bool bDontChangeTheirExistingItems, bool& bDealGoodToBeginWith, bool& bCantMatchOffer, bool& result)
{
	McpEndEqualizing();
}

static bool ReplaceIsDealWithHumanAcceptable(CivHooks::CvDealAI_IsDealWithHumanAcceptable::Original original, CvDealAI* self, CvDeal* pDeal, PlayerTypes eOtherPlayer, int& iTotalValueToMe, int& iValueImOffering, int& iValueTheyreOffering, int& iAmountOverWeWillRequest, int& iAmountUnderWeWillOffer, bool& bCantMatchOffer)
{
	return original(self, pDeal, McpEqualizingWith(eOtherPlayer), iTotalValueToMe, iValueImOffering, iValueTheyreOffering, iAmountOverWeWillRequest, iAmountUnderWeWillOffer, bCantMatchOffer);
}

static void AfterRegisterGame(lua_State* L)
{
	McpAddGameMethods(s_host, L);
}

static void UnitOrdersCancelled(CvUnit* unit, int reason)
{
	McpNoteOrdersCancelled(unit, (CivOrdersCancelledReason)reason);
}

static bool IsPluginControlledPlayer(int player)
{
	return McpIsSeat((PlayerTypes)player);
}

static bool KeepDeadSlotOpen(int player)
{
	return McpKeepDeadSlotOpen((PlayerTypes)player);
}

static void PopupRaised(int player, int popupType, int data1, int data2, int data3, bool option1, bool option2, const char* text)
{
	McpNoteSeatPopup((PlayerTypes)player, popupType, data1, data2, data3, option1, text);
}

static void NotificationAdded(int player, int notificationType, const char* message, const char* summary, int x, int y, int data1, int data2, int lookupIndex)
{
	McpNoteSeatNotification((PlayerTypes)player, message, summary);
}

static void DealResolved(int fromPlayer, int toPlayer, int actingPlayer, bool accepted, bool finalized)
{
	McpNoteDealResolved((PlayerTypes)fromPlayer, (PlayerTypes)toPlayer, (PlayerTypes)actingPlayer, accepted, finalized);
}

PLUGIN_EXPORT bool CivPluginEntry(const CivHostInfo* host, CivPluginInfo* plugin)
{
	s_host = host;
	// VC++ 2008 doesn't guard a function's static, and the game and UI threads could both build it on first use.
	McpBridge::Get();
	plugin->name = "MCP Server for Bazaar";
	plugin->version = MCP_SERVER_VERSION;
	Check(CivRegisterBefore<CivHooks::CvGame_update>(host, &BeforeGameUpdate), "before CvGame::update");
	Check(CivRegisterAfter<CivHooks::CvGame_Read>(host, &AfterGameRead), "after CvGame::Read");
	Check(host->RegisterHook(host->pluginIndex, CivHooks::CvDllPlayer_IsHuman::ID, CIV_HOOK_AFTER, (void*)&AfterDllPlayerIsHuman), "after CvDllPlayer::IsHuman");
	Check(host->RegisterHook(host->pluginIndex, CivHooks::CvDllPreGame_isHuman::ID, CIV_HOOK_AFTER, (void*)&AfterDllPreGameIsHuman), "after CvDllPreGame::isHuman");
	Check(CivRegisterBefore<CivHooks::CvDiplomacyRequests_SendRequest>(host, &BeforeSendRequest), "before CvDiplomacyRequests::SendRequest");
	Check(CivRegisterBefore<CivHooks::CvDiplomacyRequests_SendDealRequest>(host, &BeforeSendDealRequest), "before CvDiplomacyRequests::SendDealRequest");
	Check(CivRegisterBefore<CivHooks::CvDealAI_DoEqualizeDealWithHuman>(host, &BeforeEqualizeDeal), "before CvDealAI::DoEqualizeDealWithHuman");
	Check(CivRegisterAfter<CivHooks::CvDealAI_DoEqualizeDealWithHuman>(host, &AfterEqualizeDeal), "after CvDealAI::DoEqualizeDealWithHuman");
	Check(CivRegisterReplace<CivHooks::CvDealAI_IsDealWithHumanAcceptable>(host, &ReplaceIsDealWithHumanAcceptable), "replace CvDealAI::IsDealWithHumanAcceptable");
	Check(CivRegisterAfter<CivHooks::CvLuaGame_RegisterMembers>(host, &AfterRegisterGame), "after CvLuaGame::RegisterMembers");
	Check(host->RegisterHandHook(host->pluginIndex, CIV_HAND_IS_PLUGIN_CONTROLLED_PLAYER, (void*)static_cast<CivPlayerQueryFn>(&IsPluginControlledPlayer)), "IsPluginControlledPlayer");
	Check(host->RegisterHandHook(host->pluginIndex, CIV_HAND_KEEP_DEAD_SLOT_OPEN, (void*)static_cast<CivPlayerQueryFn>(&KeepDeadSlotOpen)), "KeepDeadSlotOpen");
	Check(host->RegisterHandHook(host->pluginIndex, CIV_HAND_POPUP_RAISED, (void*)static_cast<CivPopupRaisedFn>(&PopupRaised)), "PopupRaised");
	Check(host->RegisterHandHook(host->pluginIndex, CIV_HAND_NOTIFICATION_ADDED, (void*)static_cast<CivNotificationAddedFn>(&NotificationAdded)), "NotificationAdded");
	Check(host->RegisterHandHook(host->pluginIndex, CIV_HAND_DEAL_RESOLVED, (void*)static_cast<CivDealResolvedFn>(&DealResolved)), "DealResolved");
	Check(host->RegisterHandHook(host->pluginIndex, CIV_HAND_UNIT_ORDERS_CANCELLED, (void*)static_cast<CivUnitOrdersCancelledFn>(&UnitOrdersCancelled)), "UnitOrdersCancelled");
	return true;
}

PLUGIN_EXPORT void CivPluginShutdown()
{
	McpBridge::Get().Shutdown();
	McpShutdownMapDraw();
	Log("shutting down");
}

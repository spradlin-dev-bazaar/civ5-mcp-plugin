#pragma once

#include <string>
#include "picojson.h"

class CvPlayer;
class CvUnit;
class CvCity;
class CvPlot;
struct OrderData;

std::string McpFormat(const char* fmt, ...);
// One line in MCPServer.log, the mod's own diagnostic log.
void McpDiagLog(const std::string& msg);

// True while the player can still give orders this turn. Shared by the bridge's
// turn flag and the status tool so both agree.
bool McpIsPlayerTurn(const CvPlayer& kPlayer);

void McpResetBriefing();

// Helpers shared by the tool implementation files. Everything here runs on the game thread.
namespace McpTool
{
	typedef picojson::object JObj;
	typedef picojson::array JArr;
	typedef picojson::value JVal;

	JVal J(int v);
	JVal J(double v);
	JVal J(bool v);
	JVal J(const char* s);
	JVal J(const std::string& s);

	bool IsInt(const JVal& v, int& out); // a whole number that fits an int
	int ArgInt(const JObj& a, const char* k, int def);
	bool RequireInt(const JObj& a, const char* k, int& out, std::string& err);
	bool ArgBool(const JObj& a, const char* k, bool def);
	std::string ArgStr(const JObj& a, const char* k);
	bool HasArg(const JObj& a, const char* k);
	// The tile a map tool centers on: x and y when given (both or neither), else the capital,
	// else the first unit. False with err when neither works or the tile is off the map.
	bool ResolveMapCenter(CvPlayer* me, const JObj& args, int& cx, int& cy, std::string& err);

	CvPlayer* Me(std::string& err);
	// True when key names the entry by its type string or its displayed name.
	bool MatchesKey(const char* szType, const char* szDescription, const std::string& key);
	CvUnit* RequireUnit(CvPlayer* me, const JObj& args, std::string& err);
	bool RequireMoves(CvUnit* u, std::string& err);
	CvCity* RequireCity(CvPlayer* me, const JObj& args, std::string& err);
	// False, with err set, until the connected model has called set_identity.
	bool RequireIdentity(std::string& err);
	// Only while the turn is open; use for every tool that changes game state.
	CvPlayer* MeActing(std::string& err);

	// The game flags unit orders for human players that arrive outside its network
	// message path; its own handlers wrap such calls exactly like this.
	struct NetMessageScope
	{
		NetMessageScope();
		~NetMessageScope();
	};

	double Moves(int iMoves);
	bool AiProcessing();
	bool IsEnemyOf(CvPlayer* me, PlayerTypes eOther);
	bool PolicyChoiceAvailable(CvPlayer* me);

	const char* TerrainName(TerrainTypes e);
	const char* FeatureName(FeatureTypes e);
	const char* ResourceName(ResourceTypes e);
	const char* ImprovementName(ImprovementTypes e);
	const char* RouteName(RouteTypes e);
	const char* UnitTypeName(UnitTypes e);
	const char* UnitTypeKey(UnitTypes e);
	const char* BuildingName(BuildingTypes e);
	const char* TechName(TechTypes e);
	std::string BuildName(BuildTypes e);
	const char* EraName(EraTypes e);
	const char* PromotionName(PromotionTypes e);
	std::string CivName(PlayerTypes e);
	std::string StripMarkup(const std::string& s);
	std::string Localized(const char* szKey);

	UnitTypes FindUnitType(const std::string& key);
	BuildingTypes FindBuildingType(const std::string& key);
	// A unit or building a city can make, swapped for this civilization's unique
	// version when it has one; substituted says so when that happened.
	struct CityItem
	{
		UnitTypes eUnit;
		BuildingTypes eBuilding;
		std::string substituted;
	};
	CityItem FindCityItem(const CvPlayer* me, const std::string& key);
	TechTypes FindTechType(const std::string& key);
	BuildTypes FindBuildType(const std::string& key);
	PromotionTypes FindPromotionType(const std::string& key);
	PolicyTypes FindPolicyType(const std::string& key);
	PolicyBranchTypes FindPolicyBranchType(const std::string& key);
	ImprovementTypes FindImprovementType(const std::string& key);
	ResourceTypes FindResourceType(const std::string& key);
	ProjectTypes FindProjectType(const std::string& key);
	ProcessTypes FindProcessType(const std::string& key);

	// McpRefusals.cpp: why the game refuses something, in words; "" when it does not refuse.
	std::string TrainRefusal(CvCity* c, UnitTypes eUnit, bool bWillPurchase);
	std::string BuildRefusal(CvCity* c, BuildingTypes eBuilding, bool bPurchase = false);
	std::string RestRefusal(CvUnit* u, CvPlot* p, const std::string& action);
	std::string MoveRefusal(CvUnit* u, CvPlot* dest);
	std::string UpgradeRefusal(CvUnit* u);
	// Matches a civilization's short name, player name, or numeric id as text.
	PlayerTypes FindPlayer(const std::string& key);

	const char* FocusName(CityAIFocusTypes e);
	bool FindFocus(const std::string& name, CityAIFocusTypes& out);
	std::string FocusNames();

	std::string BlockingName(EndTurnBlockingTypes e);
	// Computed now, the way CvGame::updateTestEndTurn does; the engine stops
	// refreshing the player's cached value while a popup is up.
	EndTurnBlockingTypes EndTurnBlocking(CvPlayer* me);
	std::string UnitActivity(CvUnit* u);
	std::string OrderName(const OrderData* od);

	// What one of the model's units would hit on a tile, following the game's own rules:
	// an enemy city wins over its garrison; otherwise the game's choice of defender.
	struct AttackTarget
	{
		CvUnit* unit;
		CvCity* city;
		bool ranged;
	};
	bool ResolveAttackTarget(CvPlayer* me, CvUnit* u, CvPlot* p, AttackTarget& out, std::string& why);

	bool CaptureUndecided(CvCity* c);
	PlayerTypes LiberationTarget(CvPlayer* me, CvCity* c);
	std::string CaptureChoices(CvPlayer* me, CvCity* c);
	std::string CaptureDecision(CvPlayer* me, CvCity* c);
	// The game holds the turn for its own question about this only for the player on screen, who may never see it.
	std::string PendingCaptureDecision(CvPlayer* me);
	std::string EndTurnBlockingText(CvPlayer* me);
	JVal UnitJson(CvUnit* u);
	JVal CityJson(CvCity* c);
	JVal PlotJson(CvPlot* p, TeamTypes eTeam, PlayerTypes eMe, int cx, int cy);
	JVal ResearchJson(CvPlayer* me);
	JArr CitiesJson(CvPlayer* me);
	JArr UnitsJson(CvPlayer* me);
	JArr PromotionsAvailable(CvUnit* u);

	struct UnitActionDef
	{
		const char* name;
		bool (*can)(CvUnit*, CvPlot*);
		const MissionTypes (*mission)();
		const char* refusal;
		bool needsMoves; // false for the resting missions; the game queues any other mission without running it while the unit cannot move
	};
	const UnitActionDef* UnitActions(size_t& count);
	std::string UnitActionNames();
}

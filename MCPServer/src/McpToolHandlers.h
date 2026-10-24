#pragma once

#include <string>
#include "picojson.h"

// Tool handlers, one per MCP tool, registered in McpTools.cpp. All run on the game thread.
#define MCP_TOOL(name) picojson::value name(const picojson::object& args, std::string& err)

// McpTools.cpp
MCP_TOOL(ToolStatus);
MCP_TOOL(ToolOverview);
MCP_TOOL(ToolUnits);
MCP_TOOL(ToolUnitOptions);
MCP_TOOL(ToolCities);
MCP_TOOL(ToolCityOptions);
MCP_TOOL(ToolMap);
MCP_TOOL(ToolResearchOptions);
MCP_TOOL(ToolMoveUnit);
MCP_TOOL(ToolUnitAction);
MCP_TOOL(ToolSetProduction);
MCP_TOOL(ToolSetResearch);
MCP_TOOL(ToolEndTurn);
MCP_TOOL(ToolLog);
MCP_TOOL(ToolAcknowledgeMessages);
MCP_TOOL(ToolGameSettings);
MCP_TOOL(ToolMods);
MCP_TOOL(ToolRankings);
MCP_TOOL(ToolSetIdentity);
MCP_TOOL(ToolTakeSeat);
MCP_TOOL(ToolReleaseSeat);
MCP_TOOL(ToolSkipHumanTurn);
MCP_TOOL(ToolTestingMode);

// McpBriefing.cpp
MCP_TOOL(ToolBriefing);
MCP_TOOL(ToolCivs);
MCP_TOOL(ToolLookup);

// McpActions.cpp
MCP_TOOL(ToolPolicyOptions);
MCP_TOOL(ToolAdoptPolicy);
MCP_TOOL(ToolPromoteUnit);
MCP_TOOL(ToolUpgradeUnit);
MCP_TOOL(ToolAttack);
MCP_TOOL(ToolCityAttack);
MCP_TOOL(ToolBuy);
MCP_TOOL(ToolBuyTile);
MCP_TOOL(ToolSetCityFocus);
MCP_TOOL(ToolCityCitizens);
MCP_TOOL(ToolCityWorkTile);
MCP_TOOL(ToolCitySpecialist);
MCP_TOOL(ToolDeclareWar);
MCP_TOOL(ToolCityTask);

// McpDiplomacy.cpp
MCP_TOOL(ToolDiplomacy);
MCP_TOOL(ToolDealOptions);
MCP_TOOL(ToolRespondToRequest);
MCP_TOOL(ToolProposeDeal);
MCP_TOOL(ToolMakePeace);
MCP_TOOL(ToolDemand);
MCP_TOOL(ToolAnswerDeal);
MCP_TOOL(ToolDiscuss);

// McpReligion.cpp
MCP_TOOL(ToolReligionOptions);
MCP_TOOL(ToolFoundPantheon);
MCP_TOOL(ToolFoundReligion);
MCP_TOOL(ToolEnhanceReligion);
MCP_TOOL(ToolAddReformationBelief);
MCP_TOOL(ToolChooseGreatPerson);

// McpCongress.cpp
MCP_TOOL(ToolCongress);
MCP_TOOL(ToolProposeResolution);
MCP_TOOL(ToolVoteResolution);
MCP_TOOL(ToolAbstain);

// McpEspionage.cpp
MCP_TOOL(ToolSpies);
MCP_TOOL(ToolMoveSpy);
MCP_TOOL(ToolStageCoup);
MCP_TOOL(ToolStealTech);

// McpMinorCivs.cpp
MCP_TOOL(ToolCityState);
MCP_TOOL(ToolCityStateAction);

// McpCulture.cpp
MCP_TOOL(ToolChooseIdeology);
MCP_TOOL(ToolSwitchIdeology);
MCP_TOOL(ToolArchaeology);
MCP_TOOL(ToolGreatWorks);
MCP_TOOL(ToolMoveGreatWork);
MCP_TOOL(ToolTradeRouteOptions);
MCP_TOOL(ToolEstablishTradeRoute);
MCP_TOOL(ToolLaunchSpaceship);

// McpOverviews.cpp
MCP_TOOL(ToolEconomicOverview);
MCP_TOOL(ToolTradeRoutes);
MCP_TOOL(ToolMilitaryOverview);
MCP_TOOL(ToolVictoryProgress);
MCP_TOOL(ToolKnownCities);
MCP_TOOL(ToolMapImage);
// McpReligion.cpp
MCP_TOOL(ToolReligionOverview);

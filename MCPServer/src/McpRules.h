#pragma once

#include "McpToolSupport.h"

namespace McpTool
{
	// me may be NULL; with a player, units and buildings are that civilization's own versions.
	void PutBuildingRules(JObj& o, BuildingTypes e, const CvPlayer* me);
	JArr TechLeadsTo(TechTypes e);
	JVal CivilizationRules(CivilizationTypes e, LeaderHeadTypes eLeader);
}

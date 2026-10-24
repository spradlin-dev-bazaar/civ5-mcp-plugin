#include "CvGameCoreDLLPCH.h"
#include "McpMapImage.h"
#include "McpMapDraw.h"
#include "McpToolSupport.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvTeam.h"
#include "CvCity.h"
#include "CvUnit.h"
#include "CvPlot.h"
#include "CvMap.h"
#include "CvGlobals.h"
#include "CvInfos.h"
#include "CvGameCoreUtils.h"
#include <set>

using namespace McpTool;

namespace
{
	unsigned Rgb(int r, int g, int b) { return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b; }

	unsigned Darker(unsigned c, int by)
	{
		int r = std::max(0, (int)((c >> 16) & 255) - by), g = std::max(0, (int)((c >> 8) & 255) - by), b = std::max(0, (int)(c & 255) - by);
		return Rgb(r, g, b);
	}

	unsigned TerrainColor(const CvPlot* p)
	{
		if(p->isMountain()) return Rgb(96, 90, 86);
		CvTerrainInfo* info = GC.getTerrainInfo(p->getTerrainType());
		std::string type = info ? info->GetType() : "";
		unsigned c = Rgb(120, 165, 90);
		if(type == "TERRAIN_PLAINS") c = Rgb(188, 156, 88);
		else if(type == "TERRAIN_DESERT") c = Rgb(240, 226, 120);
		else if(type == "TERRAIN_TUNDRA") c = Rgb(152, 170, 168);
		else if(type == "TERRAIN_SNOW") c = Rgb(236, 236, 242);
		else if(type == "TERRAIN_COAST") c = Rgb(120, 178, 228);
		else if(type == "TERRAIN_OCEAN") c = Rgb(52, 92, 176);
		if(p->isHills()) c = Darker(c, 32);
		return c;
	}

	std::string FeatureGlyph(const CvPlot* p)
	{
		if(p->IsNaturalWonder()) return "NW";
		if(p->getFeatureType() == NO_FEATURE) return "";
		CvFeatureInfo* info = GC.getFeatureInfo(p->getFeatureType());
		std::string type = info ? info->GetType() : "";
		if(type == "FEATURE_FOREST") return "F";
		if(type == "FEATURE_JUNGLE") return "J";
		if(type == "FEATURE_MARSH") return "M";
		if(type == "FEATURE_OASIS") return "O";
		if(type == "FEATURE_FLOOD_PLAINS") return "fp";
		if(type == "FEATURE_ICE") return "ice";
		if(type == "FEATURE_FALLOUT") return "X";
		if(type == "FEATURE_ATOLL") return "atl";
		return "?";
	}

	unsigned OwnerColor(PlayerTypes e)
	{
		CvPlayerColorInfo* pc = GC.GetPlayerColorInfo(GET_PLAYER(e).getPlayerColor());
		CvColorInfo* ci = pc ? GC.GetColorInfo((ColorTypes)pc->GetColorTypePrimary()) : NULL;
		if(!ci) return Rgb(200, 200, 200);
		const CvColorA& c = ci->GetColor();
		return Rgb((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255));
	}

	// Cuts to at most n characters without splitting a UTF-8 sequence.
	std::string Cut(const std::string& s, size_t n)
	{
		size_t i = 0, chars = 0;
		while(i < s.size() && chars < n)
		{
			unsigned char c = (unsigned char)s[i];
			i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
			++chars;
		}
		return s.substr(0, std::min(i, s.size()));
	}

	std::string Abbrev(const std::string& name)
	{
		if(name.find(' ') == std::string::npos) return Cut(name, 3);
		std::string out;
		bool bStart = true;
		for(size_t i = 0; i < name.size() && out.size() < 3; ++i)
		{
			if(name[i] == ' ') { bStart = true; continue; }
			if(bStart) out += name[i];
			bStart = false;
		}
		return out;
	}

	int EdgeOf(DirectionTypes d)
	{
		switch(d)
		{
		case DIRECTION_NORTHEAST: return 0;
		case DIRECTION_EAST: return 1;
		case DIRECTION_SOUTHEAST: return 2;
		case DIRECTION_SOUTHWEST: return 3;
		case DIRECTION_WEST: return 4;
		default: return 5;
		}
	}
}

std::string McpBase64(const std::string& bytes)
{
	static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	size_t i = 0;
	for(; i + 2 < bytes.size(); i += 3)
	{
		unsigned v = ((unsigned char)bytes[i] << 16) | ((unsigned char)bytes[i + 1] << 8) | (unsigned char)bytes[i + 2];
		out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
	}
	if(i < bytes.size())
	{
		unsigned v = (unsigned char)bytes[i] << 16;
		if(i + 1 < bytes.size()) v |= (unsigned char)bytes[i + 1] << 8;
		out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
		out += (i + 1 < bytes.size()) ? tbl[(v >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

std::string McpRenderMapPng(PlayerTypes eMe, int cx, int cy, int radius, std::string& summary, JArr& cities, JArr& units)
{
	CvMap& map = GC.getMap();
	TeamTypes eTeam = GET_PLAYER(eMe).getTeam();
	CvTeam& myTeam = GET_TEAM(eTeam);
	int x0 = cx - radius - 1, y1 = cy + radius;

	std::vector<McpMapHex> hexes;
	std::set<PlayerTypes> owners;
	int iUnexplored = 0, iFogged = 0;
	for(int y = cy - radius; y <= cy + radius; ++y)
		for(int x = x0; x <= cx + radius + 1; ++x)
		{
			if(plotDistance(cx, cy, x, y) > radius) continue;
			CvPlot* p = map.plot(x, y);
			if(!p) continue;
			McpMapHex h;
			h.x = x;
			h.y = y;
			if(!p->isRevealed(eTeam))
			{
				++iUnexplored;
				h.unexplored = true;
				hexes.push_back(h);
				continue;
			}
			h.fill = TerrainColor(p);
			h.fogged = !p->isVisible(eTeam);
			if(h.fogged) ++iFogged;
			h.riverE = p->isWOfRiver();
			h.riverSE = p->isNWOfRiver();
			h.riverSW = p->isNEOfRiver();
			h.coords = McpFormat("%d,%d", p->getX(), p->getY());

			RouteTypes eRoute = p->getRevealedRouteType(eTeam);
			h.hasRoad = eRoute != NO_ROUTE;
			PlayerTypes eOwner = p->getRevealedOwner(eTeam);
			if(eOwner != NO_PLAYER) { h.owned = true; h.borderColor = OwnerColor(eOwner); owners.insert(eOwner); }
			for(int d = 0; d < NUM_DIRECTION_TYPES; ++d)
			{
				CvPlot* n = plotDirection(p->getX(), p->getY(), (DirectionTypes)d);
				bool bKnown = n && n->isRevealed(eTeam);
				int k = EdgeOf((DirectionTypes)d);
				h.road[k] = eRoute != NO_ROUTE && bKnown && n->getRevealedRouteType(eTeam) != NO_ROUTE;
				h.border[k] = eOwner != NO_PLAYER && !(bKnown && n->getRevealedOwner(eTeam) == eOwner);
			}

			ResourceTypes eRes = p->getResourceType(eTeam);
			if(eRes != NO_RESOURCE) h.topLeft = Cut(ResourceName(eRes), 6);
			h.topRight = FeatureGlyph(p);
			if(p->isMountain()) h.centerLabel = "MTN";
			else if(p->isHills()) h.topRight += "h";
			CvCity* city = p->getPlotCity();
			if(city && !city->isRevealed(eTeam, false)) city = NULL;
			ImprovementTypes eImp = p->getRevealedImprovementType(eTeam);
			if(eImp != NO_IMPROVEMENT && !city)
			{
				std::string name = ImprovementName(eImp);
				for(size_t k = 0; k < name.size(); ++k) name[k] = (char)tolower((unsigned char)name[k]);
				h.bottomLeft = Cut(name, 5);
			}

			if(city)
			{
				// Under fog the city is shown as last seen: its owner then, grey if it has changed hands since.
				PlayerTypes eCityOwner = h.fogged ? eOwner : city->getOwner();
				h.city = true;
				h.cityColor = eCityOwner != NO_PLAYER ? OwnerColor(eCityOwner) : Rgb(120, 120, 120);
				h.cityLabel = Cut(city->getName(), 9);
				if(h.cityLabel.size() < city->getName().size()) h.cityLabel = Cut(city->getName(), 8) + ".";
				if(eCityOwner != NO_PLAYER) owners.insert(eCityOwner);
				JObj co;
				co["name"] = J(city->getName().c_str());
				co["x"] = J(x);
				co["y"] = J(y);
				co["owner"] = J(eCityOwner != NO_PLAYER ? CivName(eCityOwner) : "unknown: it changed hands while out of sight");
				if(h.fogged) co["out_of_sight"] = J(true);
				cities.push_back(JVal(co));
			}
			else if(!h.fogged)
			{
				for(int k = 0; k < p->getNumUnits(); ++k)
				{
					CvUnit* u = p->getUnitByIndex(k);
					if(!u || u->isInvisible(eTeam, false)) continue;
					owners.insert(u->getOwner());
					JObj uo;
					if(h.units.size() >= 3)
					{
						++h.extraUnits;
						uo["x"] = J(x);
						uo["y"] = J(y);
						uo["type"] = J(UnitTypeName(u->getUnitType()));
						uo["owner"] = J(CivName(u->getOwner()));
						uo["relation"] = J(u->getOwner() == eMe ? "yours" : u->getTeam() == eTeam ? "teammate" : myTeam.isAtWar(u->getTeam()) ? "enemy" : "other");
						units.push_back(JVal(uo));
						continue;
					}
					McpMapUnit mu;
					mu.label = Abbrev(UnitTypeName(u->getUnitType()));
					mu.fill = OwnerColor(u->getOwner());
					const char* relation = u->getOwner() == eMe ? "yours" : u->getTeam() == eTeam ? "teammate" : myTeam.isAtWar(u->getTeam()) ? "enemy" : "other";
					mu.ring = u->getOwner() == eMe ? Rgb(255, 255, 255)
					        : u->getTeam() == eTeam ? Rgb(60, 230, 90)
					        : myTeam.isAtWar(u->getTeam()) ? Rgb(240, 40, 40)
					        : Rgb(150, 150, 150);
					h.units.push_back(mu);
					uo["x"] = J(x);
					uo["y"] = J(y);
					uo["type"] = J(UnitTypeName(u->getUnitType()));
					uo["owner"] = J(CivName(u->getOwner()));
					uo["relation"] = J(relation);
					units.push_back(JVal(uo));
				}
			}
			hexes.push_back(h);
		}

	std::vector<McpMapLegendEntry> legend;
	for(std::set<PlayerTypes>::const_iterator it = owners.begin(); it != owners.end(); ++it)
	{
		McpMapLegendEntry e;
		e.color = OwnerColor(*it);
		bool bKnown = GET_PLAYER(*it).getTeam() == eTeam || myTeam.isHasMet(GET_PLAYER(*it).getTeam());
		e.name = bKnown ? CivName(*it) : "unmet civilization";
		if(*it == eMe) e.name += " (you)";
		else if(GET_PLAYER(*it).getTeam() == eTeam) e.name += " (teammate)";
		else if(myTeam.isAtWar(GET_PLAYER(*it).getTeam())) e.name += " (at war)";
		legend.push_back(e);
	}
	std::string footer = McpFormat("center %d,%d  radius %d  hatched = out of sight now  ? = unexplored", cx, cy, radius);

	std::string err;
	std::string png = McpDrawHexMapPng(hexes, legend, footer, x0, y1, 2 * radius + 3, 2 * radius + 1, err);
	if(png.empty()) { summary = err; return ""; }
	McpSaveMapCopy(png);
	summary = McpFormat("%d tiles drawn around (%d,%d); %d of them out of sight now; %d unexplored, marked ?", (int)hexes.size() - iUnexplored, cx, cy, iFogged, iUnexplored);
	return png;
}

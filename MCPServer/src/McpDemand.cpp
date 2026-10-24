#include "CvGameCoreDLLPCH.h"
#include "McpDemand.h"
#include "CvGame.h"
#include "CvPlayer.h"
#include "CvDealClasses.h"
#include "CvDealAI.h"
#include "CvDiplomacyAI.h"
#include "CvTreasury.h"
#include "CvGlobals.h"
#include "CvInfos.h"

// VMC's CvDealAI::DoHumanDemand, which only takes demands from the player on screen. Keep it in step with VMC's.
DemandResponseTypes McpHumanDemand(CvPlayer& kAI, CvDeal* pDeal, PlayerTypes eFromPlayer)
{
	DemandResponseTypes eResponse = NO_DEMAND_RESPONSE_TYPE;

	PlayerTypes eMyPlayer = kAI.GetID();

	int iValueWillingToGiveUp = 0;

	CvDiplomacyAI* pDiploAI = GET_PLAYER(eMyPlayer).GetDiplomacyAI();
	
		// Too soon for another demand?
		if(pDiploAI->IsDemandTooSoon(eFromPlayer))
			eResponse = DEMAND_RESPONSE_REFUSE_TOO_SOON;

		// Not too soon for a demand
		else
		{
			MajorCivApproachTypes eApproach = pDiploAI->GetMajorCivApproach(eFromPlayer, /*bHideTrueFeelings*/ true);
			StrengthTypes eMilitaryStrength = pDiploAI->GetPlayerMilitaryStrengthComparedToUs(eFromPlayer);
			AggressivePostureTypes eMilitaryPosture = pDiploAI->GetMilitaryAggressivePosture(eFromPlayer);
			PlayerProximityTypes eProximity = GET_PLAYER(eMyPlayer).GetProximityToPlayer(eFromPlayer);

			// Unforgivable: AI will never give in
			if(pDiploAI->GetMajorCivOpinion(eFromPlayer) == MAJOR_CIV_OPINION_UNFORGIVABLE)
				eResponse = DEMAND_RESPONSE_REFUSE_HOSTILE;

			// Hostile: AI will never give in
			else if(eApproach == MAJOR_CIV_APPROACH_HOSTILE)
				eResponse = DEMAND_RESPONSE_REFUSE_HOSTILE;

			// Our military is stronger: AI will never give in
			else if(eMilitaryStrength < STRENGTH_AVERAGE)
				eResponse = DEMAND_RESPONSE_REFUSE_WEAK;

			// They are very far away and have no units near us (from what we can tell): AI will never give in
			else if(eProximity <= PLAYER_PROXIMITY_FAR && eMilitaryPosture == AGGRESSIVE_POSTURE_NONE)
				eResponse = DEMAND_RESPONSE_REFUSE_WEAK;

			// Willing to give in to demand
			else
			{
				// Initial odds of giving in to ANY demand are based on the player's boldness (which is also tied to the player's likelihood of going for world conquest)
				int iOddsOfGivingIn = (10 - pDiploAI->GetBoldness()) * 10;

				iValueWillingToGiveUp = 0;
				// If we're afraid we're more likely to give in
				if(eApproach == MAJOR_CIV_APPROACH_AFRAID)
				{
					iOddsOfGivingIn += 50;
					iValueWillingToGiveUp += 200;
				}
				// Not afraid
				else
				{
					// How strong are they compared to us?
					switch(eMilitaryStrength)
					{
					case STRENGTH_PATHETIC:
						iOddsOfGivingIn += -100;
						iValueWillingToGiveUp += 10;
						break;
					case STRENGTH_WEAK:
						iOddsOfGivingIn += -100;
						iValueWillingToGiveUp += 10;
						break;
					case STRENGTH_POOR:
						iOddsOfGivingIn += -100;
						iValueWillingToGiveUp += 20;
						break;
					case STRENGTH_AVERAGE:
						iOddsOfGivingIn += -10;
						iValueWillingToGiveUp += 50;
						break;
					case STRENGTH_STRONG:
						iOddsOfGivingIn += 10;
						iValueWillingToGiveUp += 120;
						break;
					case STRENGTH_POWERFUL:
						iOddsOfGivingIn += 20;
						iValueWillingToGiveUp += 200;
						break;
					case STRENGTH_IMMENSE:
						iOddsOfGivingIn += 35;
						iValueWillingToGiveUp += 200;
						break;
					default:
						break;
					}
				}

				// IMPORTANT NOTE: This APPEARS to be very bad for multiplayer, but the only changes made to the game state are the fact that the human
				// made a demand, and if the deal went through. These are both sent over the network later in this function.

				int iAsyncRand = GC.getGame().getAsyncRandNum(100, "Deal AI: ASYNC RAND call to determine if AI will give into a human demand.");

				// Are they going to say no matter what?
				if(iAsyncRand > iOddsOfGivingIn)
					eResponse = DEMAND_RESPONSE_REFUSE_HOSTILE;
		}
	}

	// Possibility exists that the AI will accept
	if(eResponse == NO_DEAL_RESPONSE_TYPE)
	{
		int iValueDemanded = 0;

		int iGPT = kAI.GetTreasury()->CalculateBaseNetGold();
		int iTempGold;
		int iModdedGoldValue;

		// Loop through items in this deal
		TradedItemList::iterator it;
		for(it = pDeal->m_TradedItems.begin(); it != pDeal->m_TradedItems.end(); ++it)
		{
			// Item from this AI
			if(it->m_eFromPlayer == eMyPlayer)
			{
				switch(it->m_eItemType)
				{
					// Gold
				case TRADE_ITEM_GOLD:
				{
					iTempGold = it->m_iData1;
					if (iGPT > 0)
						iModdedGoldValue = iTempGold * 10 / iGPT;
					else
						iModdedGoldValue = 0;

					iValueDemanded += max(iTempGold, iModdedGoldValue);
					break;
				}

				// GPT
				case TRADE_ITEM_GOLD_PER_TURN:
				{
					iValueDemanded += (it->m_iData1 * it->m_iDuration * 80 / 100);
					break;
				}

				// Resources
				case TRADE_ITEM_RESOURCES:
				{
					ResourceTypes eResource = (ResourceTypes) it->m_iData1;
					ResourceUsageTypes eUsage = GC.getResourceInfo(eResource)->getResourceUsage();

					if(eUsage == RESOURCEUSAGE_LUXURY)
						iValueDemanded += 200;
					else if(eUsage == RESOURCEUSAGE_STRATEGIC)
						iValueDemanded += (40 * it->m_iData2);

					break;
				}

				// Open Borders
				case TRADE_ITEM_OPEN_BORDERS:
				{
					iValueDemanded += 50;
					break;
				}

				case TRADE_ITEM_CITIES:
				case TRADE_ITEM_DEFENSIVE_PACT:
				case TRADE_ITEM_RESEARCH_AGREEMENT:
				case TRADE_ITEM_PERMANENT_ALLIANCE:
				case TRADE_ITEM_THIRD_PARTY_PEACE:
				case TRADE_ITEM_THIRD_PARTY_WAR:
				case TRADE_ITEM_THIRD_PARTY_EMBARGO:
				default:
					eResponse = DEMAND_RESPONSE_REFUSE_TOO_MUCH;
					break;
				}
			}
		}

		// No illegal items in the demand
		if(eResponse == NO_DEAL_RESPONSE_TYPE)
		{
			if(iValueDemanded <= iValueWillingToGiveUp)
				eResponse = DEMAND_RESPONSE_ACCEPT;
			else
				eResponse = DEMAND_RESPONSE_REFUSE_TOO_MUCH;
		}
	}

	// Have to sent AI response through the network  - it affects AI behavior
	if(eFromPlayer == GC.getGame().getActivePlayer())
		GC.getGame().DoFromUIDiploEvent(FROM_UI_DIPLO_EVENT_HUMAN_DEMAND, eMyPlayer, /*iData1*/ eResponse, -1);
	else
		pDiploAI->DoFromUIDiploEvent(eFromPlayer, FROM_UI_DIPLO_EVENT_HUMAN_DEMAND, eResponse, -1);

	// Demand agreed to
	if(eResponse == DEMAND_RESPONSE_ACCEPT)
	{
		CvDeal kDeal = *pDeal;
		if(eFromPlayer != GC.getGame().getActivePlayer())
		{
			kAI.GetDealAI()->DoAcceptedDemand(eFromPlayer, kDeal);
			return eResponse;
		}
		//gDLL->sendNetDealAccepted(eFromPlayer, GetPlayer()->GetID(), kDeal, -1, -1, -1);
		GC.GetEngineUserInterface()->SetDealInTransit(true);

		auto_ptr<ICvDeal1> pDllDeal = GC.WrapDealPointer(&kDeal);
		gDLL->sendNetDemandAccepted(eFromPlayer, eMyPlayer, pDllDeal.get());
	}

	return eResponse;
}

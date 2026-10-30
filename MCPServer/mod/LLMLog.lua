-- MCP Server for Bazaar UI add-in: the AI Log window (Game.GetLLMLog) and its chat with teammate
-- seats, popup capture, leader screens left for a model, the mod list, the turn banner for seats,
-- and tile coordinates.
-- Every game popup is reported to the DLL (Game.LLMNotePopup). It stays open
-- until the model acknowledges it; the DLL then names the dialog in Game.LLMDialogsToClose.
include("InstanceManager");
include("IconSupport");

-- Bazaar can run this mod while its plugin is not loaded (refused, failed or skipped).
if Game.GetLLMLog == nil then
	Controls.OpenButton:SetHide(true);
	print("LLMLog: the MCP server plugin is not running, so the AI Log is off");
	return;
end

local MAX_LINES_SHOWN = 400;

-- The DLL tags every log entry with one of these categories (src/McpLogText.h);
-- entries tagged "always" are shown whatever is filtered.
local CATEGORIES = {
	{ key = "reasoning", label = "Notes" },
	{ key = "chat", label = "Chat" },
	{ key = "turn", label = "Turns" },
	{ key = "movement", label = "Military moves" },
	{ key = "military", label = "Military orders" },
	{ key = "combat", label = "Combat" },
	{ key = "workers", label = "Workers" },
	{ key = "settlers", label = "Settlers" },
	{ key = "civilian", label = "Other civilians" },
	{ key = "city", label = "Cities" },
	{ key = "empire", label = "Research, policies" },
	{ key = "diplomacy", label = "Diplomacy" },
	{ key = "messages", label = "Game messages" },
	{ key = "notifications", label = "Notifications" },
};

local g_Lines = InstanceManager:new("LineInstance", "Root", Controls.LineStack);
local g_Filters = InstanceManager:new("FilterInstance", "Root", Controls.FilterStack);
-- Named after the earlier Local MCP Server DLL mod, so its players keep their saved filters.
local g_Settings = Modding.OpenUserData("LocalMCPServerDLL_Log", 1);
local g_Shown = {};
local g_LastSeq = -1;
local g_LastHidden = nil;
local g_SinceRefresh = 0;
local g_SinceIdentity = 0;
local g_IdentityLabel = nil;
local g_RepliesHeard = nil; -- replies counted at the last check; a rise plays the chat sound

-- A connected model names itself (set_identity), e.g. Claude / Opus / 5.5. The button
-- carries that name while exactly one model has; "AI" otherwise. It also counts the
-- chat replies written since the window was last open.
local function UpdateIdentity()
	local count, model, label = Game.GetLLMIdentity();
	if count ~= 1 then
		label = (count > 1) and string.format("%d AI players", count) or "";
	end
	local unread, total = Game.LLMChatUnread();
	if g_RepliesHeard ~= nil and total > g_RepliesHeard then
		Events.AudioPlay2DSound("AS2D_IF_MP_CHAT_DING"); -- the game's multiplayer chat ding, never heard in single player
	end
	g_RepliesHeard = total;
	local key = label .. "|" .. unread;
	if key == g_IdentityLabel then
		return;
	end
	g_IdentityLabel = key;
	local short = (count == 1 and model ~= "") and string.sub(model, 1, 12) or "AI";
	Controls.OpenButtonLabel:SetText(short .. " Log" .. ((unread > 0) and string.format(" (%d)", unread) or ""));
	Controls.OpenButton:SetToolTipString("Show what " .. short .. " is thinking");
	Controls.Title:SetText(((label ~= "") and label or "AI") .. " Log");
end

-- bToBottom: jump to the newest line; otherwise only follow it if the reader is already there.
-- Lines are prefixed with the writer's name and civilization once more than one model has written.
local function WriterPrefix(entries)
	local writers = {};
	local count = 0;
	for _, entry in ipairs(entries) do
		if entry.Who ~= "" and not writers[entry.Who] then
			writers[entry.Who] = true;
			count = count + 1;
		end
	end
	if count < 2 then
		return function() return ""; end;
	end
	return function(entry)
		if entry.Who == "" then
			return "";
		elseif entry.Civ ~= "" then
			return string.format("%s (%s): ", entry.Who, entry.Civ);
		end
		return string.format("%s: ", entry.Who);
	end;
end

-- Each line gets one "Go to:" button per tile it names (the LineInstance has room for 6).
local MAX_LINKS = 6;

local function GoToTile(x, y)
	UI.LookAt(Map.GetPlot(x, y), 0);
end

-- The DLL writes tiles as "(x, y)"; models often write "(x,y)".
local function TilesNamed(line)
	local seen = {};
	local tiles = {};
	for x, y in string.gmatch(line, "%((%d+),%s*(%d+)%)") do
		local key = x .. "," .. y;
		if not seen[key] and Map.GetPlot(tonumber(x), tonumber(y)) ~= nil then
			seen[key] = true;
			table.insert(tiles, { x = tonumber(x), y = tonumber(y), label = string.format("(%s, %s)", x, y) });
		end
	end
	return tiles;
end

local function AddLine(line)
	local instance = g_Lines:GetInstance();
	instance.Text:SetText(line);
	local tiles = TilesNamed(line);
	for i = 1, MAX_LINKS do
		local button = instance["Link" .. i];
		local tile = tiles[i];
		button:SetHide(tile == nil);
		if tile ~= nil then
			button:SetText(tile.label);
			button:SetToolTipString("Show this tile on the map");
			button:SetVoids(tile.x, tile.y);
			button:RegisterCallback(Mouse.eLClick, GoToTile);
		end
	end
	instance.Links:SetHide(#tiles == 0);
	instance.Links:CalculateSize();
	instance.Root:CalculateSize();
end

local function Refresh(bToBottom)
	local seq, entries, hidden = Game.GetLLMLog();
	if seq == g_LastSeq and hidden == g_LastHidden then
		return;
	end
	g_LastSeq = seq;
	g_LastHidden = hidden;
	local bFollow = bToBottom or Controls.ScrollPanel:GetRatio() >= 1 or Controls.ScrollPanel:GetScrollValue() > 0.99;

	local lines = {};
	if hidden > 0 then
		if hidden == 1 then
			table.insert(lines, "One AI player is your opponent, so its log is hidden.");
		else
			table.insert(lines, string.format("%d AI players are your opponents, so their logs are hidden.", hidden));
		end
		table.insert(lines, "Their full thinking is in the windows they run in, and in AIThinking.log in the game's Logs folder.");
	end
	entries = entries or {};
	local prefix = WriterPrefix(entries);
	for _, entry in ipairs(entries) do
		if entry.Category == "always" or g_Shown[entry.Category] ~= false then
			local color = nil;
			if entry.Category == "chat" then
				color = (string.sub(entry.Text, 1, 4) == "You:") and "[COLOR_CYAN]" or "[COLOR_YELLOW]";
			end
			local text = prefix(entry) .. entry.Text;
			repeat
				local before = text;
				text = text:gsub("%[NEWLINE%]%s*%[NEWLINE%]", "[NEWLINE]"):gsub("%s*%[NEWLINE%]%s*$", "");
			until text == before;
			for line in string.gmatch(text .. "\n", "(.-)\n") do
				if color ~= nil and line ~= "" then
					line = color .. line .. "[ENDCOLOR]";
				end
				table.insert(lines, line);
			end
		end
	end

	g_Lines:ResetInstances();
	if #lines == 0 then
		AddLine("(nothing logged yet)");
	else
		local first = math.max(1, #lines - MAX_LINES_SHOWN + 1);
		if first > 1 then
			AddLine(string.format("(%d older lines not shown)", first - 1));
		end
		for i = first, #lines do
			local line = lines[i];
			if line == "" then
				line = " ";
			end
			AddLine(line);
		end
	end

	Controls.LineStack:CalculateSize();
	Controls.LineStack:SetSizeY(Controls.LineStack:GetSizeY() + 8); -- room for the last line's descenders
	Controls.LineStack:ReprocessAnchoring();
	Controls.ScrollPanel:CalculateInternalSize();
	if bFollow then
		Controls.ScrollPanel:SetScrollValue(1);
	end
end

-- The chat row: a message goes to one AI seat on the human's team, picked in the pulldown.
-- The row is hidden when there is no such seat.
local g_Teammates = {};
local g_TeammatesKey = nil;
local g_ChatTarget = nil;

local function SelectChatTarget(index)
	local teammate = g_Teammates[index];
	if teammate == nil then
		return;
	end
	g_ChatTarget = teammate.Player;
	Controls.ChatTarget:GetButton():SetText("To: " .. teammate.Name);
end

local function UpdateTeammates()
	local teammates = Game.GetLLMTeammates() or {};
	local parts = {};
	for _, teammate in ipairs(teammates) do
		table.insert(parts, teammate.Player .. "=" .. teammate.Name);
	end
	local key = table.concat(parts, ";");
	if key == g_TeammatesKey then
		return;
	end
	g_TeammatesKey = key;
	g_Teammates = teammates;
	Controls.ChatRow:SetHide(#teammates == 0);
	if #teammates == 0 then
		g_ChatTarget = nil;
		return;
	end
	Controls.ChatTarget:ClearEntries();
	local selected = 1;
	for i, teammate in ipairs(teammates) do
		local entry = {};
		Controls.ChatTarget:BuildEntry("InstanceOne", entry);
		entry.Button:SetText("To: " .. teammate.Name);
		entry.Button:SetVoid1(i);
		if teammate.Player == g_ChatTarget then
			selected = i;
		end
	end
	Controls.ChatTarget:CalculateInternals();
	SelectChatTarget(selected);
end

local function SendChat(text)
	text = (text or ""):gsub("^%s+", ""):gsub("%s+$", "");
	if text == "" or g_ChatTarget == nil then
		return;
	end
	Game.LLMChat(g_ChatTarget, text);
	Controls.ChatEntry:ClearString();
end

Controls.ChatTarget:RegisterSelectionCallback(SelectChatTarget);
Controls.ChatEntry:RegisterCallback(SendChat);
Controls.ChatSend:RegisterCallback(Mouse.eLClick, function() SendChat(Controls.ChatEntry:GetText()); end);

for _, category in ipairs(CATEGORIES) do
	local key = category.key;
	g_Shown[key] = tostring(g_Settings.GetValue("Show_" .. key)) ~= "0";
	local filter = g_Filters:GetInstance();
	filter.Box:GetTextButton():SetText(category.label);
	filter.Box:SetCheck(g_Shown[key]);
	filter.Box:RegisterCheckHandler(function(bChecked)
		g_Shown[key] = bChecked;
		g_Settings.SetValue("Show_" .. key, bChecked and "1" or "0");
		g_LastSeq = -1;
		Refresh(true);
	end);
end
Controls.FilterStack:CalculateSize();
Controls.FilterStack:ReprocessAnchoring();

local function Open()
	g_LastSeq = -1;
	UpdateTeammates();
	Refresh(true);
	Game.LLMChatSeen();
	Controls.Popup:SetHide(false);
end

local function Close()
	Controls.Popup:SetHide(true);
end

Controls.OpenButton:RegisterCallback(Mouse.eLClick, Open);
Controls.CloseButton:RegisterCallback(Mouse.eLClick, Close);

local g_PendingDismiss = {};
local g_Stopped = false;

local function GoodyText(popupInfo)
	local goody = GameInfo.GoodyHuts[popupInfo.Data1];
	if goody == nil then
		return "";
	end
	local text = Locale.ConvertTextKey(goody.Description);
	if goody.Gold > 0 or goody.Culture > 0 then
		text = Locale.ConvertTextKey(goody.Description, popupInfo.Data2);
	end
	return text;
end

local function OnGamePopup(popupInfo)
	if g_Stopped then
		return;
	end
	local text = popupInfo.Text or "";
	if popupInfo.Type == ButtonPopupTypes.BUTTONPOPUP_GOODY_HUT_REWARD then
		text = GoodyText(popupInfo);
	elseif popupInfo.Type == ButtonPopupTypes.BUTTONPOPUP_CITY_STATE_MESSAGE then
		local message = UI.GetTempString();
		if message ~= nil and message ~= "" then
			text = message;
		end
	end
	Game.LLMNotePopup(popupInfo.Type, popupInfo.Data1 or -1, popupInfo.Data2 or -1, popupInfo.Data3 or -1, popupInfo.Option1 == true, text);
end
Events.SerialEventGameMessagePopup.Add(OnGamePopup);

-- A leader screen holds the game until someone leaves it, which a model playing the civilization on screen
-- cannot do: the plugin passes the leader's words to the model, and the screen is left here, the way its
-- own Back button does, once it shows. The game hides this window's update while a leader screen is up,
-- so the watch runs on the application's frames. A conversation the human opens, by clicking a leader or a
-- spy notification, stays theirs until the leader view closes or the next turn starts; the AI answers a
-- model's discuss with DISCUSS_HUMAN_INVOKED too, so that state does not mark the human.
local LEADER_SCREENS = { "/LeaderHeadRoot/DiscussionDialog", "/LeaderHeadRoot" };
local HUMAN_OPENED = {
	[DiploUIStateTypes.DIPLO_UI_STATE_DEFAULT_ROOT] = true,
	[DiploUIStateTypes.DIPLO_UI_STATE_CONFRONT_YOU_KILLED_MY_SPY] = true,
};
local LEADER_WATCH_FRAMES = 300;
local g_LeaderWatch = 0;
local g_HumanTalking = false;

local function OnLeaderMessage(iPlayer, iDiploUIState, szLeaderMessage)
	if g_Stopped then
		return;
	end
	if HUMAN_OPENED[iDiploUIState] then
		g_HumanTalking = true;
	end
	if g_HumanTalking then
		g_LeaderWatch = 0;
		return;
	end
	if Game.LLMTakeLeaderMessage(iPlayer, iDiploUIState, szLeaderMessage or "") then
		g_LeaderWatch = LEADER_WATCH_FRAMES;
	end
end
Events.AILeaderMessage.Add(OnLeaderMessage);

-- Some screens the human opens never enter the leader view, so the next turn start ends their conversation too.
local function OnHumanDoneTalking()
	g_HumanTalking = false;
end
Events.LeavingLeaderViewMode.Add(OnHumanDoneTalking);
Events.ActivePlayerTurnStart.Add(OnHumanDoneTalking);

local function LeaveLeaderScreens()
	if g_LeaderWatch <= 0 then
		return;
	end
	if g_Stopped or g_HumanTalking then
		g_LeaderWatch = 0;
		return;
	end
	g_LeaderWatch = g_LeaderWatch - 1;
	local left = false;
	for _, path in ipairs(LEADER_SCREENS) do
		local control = ContextPtr:LookUpControl(path);
		if control ~= nil and not control:IsHidden() then
			UIManager:DequeuePopup(control);
			left = true;
		end
	end
	if left then
		UI.SetLeaderHeadRootUp(false);
		UI.RequestLeaveLeader();
		print("LLMLog: left the leader screen for the model playing this civilization");
	end
end
Events.LocalMachineAppUpdate.Add(LeaveLeaderScreens);

local function CollectAcknowledged()
	local names = Game.LLMDialogsToClose();
	if names == nil or names == "" then
		return;
	end
	for name in string.gmatch(names, "[^\n]+") do
		if #g_PendingDismiss >= 50 then
			print("LLMLog: too many dialogs waiting to be closed; dropping " .. g_PendingDismiss[1].name);
			table.remove(g_PendingDismiss, 1);
		end
		table.insert(g_PendingDismiss, { name = name, waited = 0 });
	end
end

-- An acknowledged dialog may still be waiting behind other dialogs, so each entry
-- stays until its dialog has actually been shown and closed. If the human closed
-- it by hand it never shows again; the entry is dropped so it cannot close the
-- next dialog of that kind unread. A dialog stuck behind others for that long is
-- left for the human to close, which is the safer failure.
local MAX_DISMISS_WAIT_SECONDS = 10;

local function DismissPending(fDeltaTime)
	local remaining = {};
	local closed = {};
	for _, pending in ipairs(g_PendingDismiss) do
		local control = ContextPtr:LookUpControl("/InGame/" .. pending.name);
		if control == nil then
			print("LLMLog: no dialog named " .. pending.name);
		elseif not control:IsHidden() and not closed[pending.name] then
			UIManager:DequeuePopup(control);
			closed[pending.name] = true;
		else
			pending.waited = pending.waited + fDeltaTime;
			if pending.waited > MAX_DISMISS_WAIT_SECONDS then
				print("LLMLog: " .. pending.name .. " was not shown within " .. MAX_DISMISS_WAIT_SECONDS .. " s; dropping it");
			else
				table.insert(remaining, pending);
			end
		end
	end
	g_PendingDismiss = remaining;
end

UpdateIdentity();

local MAX_MOD_DESCRIPTION = 400;

-- One line per mod: "Name (v N)", a tab, then the description flattened to one line.
local function ReportActiveMods()
	local mods = {};
	for _, v in ipairs(Modding.GetActivatedMods()) do
		local name = Modding.GetModProperty(v.ID, v.Version, "Name") or v.ID;
		local description = Modding.GetModProperty(v.ID, v.Version, "Description") or "";
		description = description:gsub("%[NEWLINE%]", " "):gsub("%s+", " "):gsub("^ ", ""):gsub(" $", "");
		if #description > MAX_MOD_DESCRIPTION then
			description = description:sub(1, MAX_MOD_DESCRIPTION) .. "...";
		end
		table.insert(mods, string.format("%s (v %d)\t%s", (name:gsub("%s+", " ")), v.Version, description));
	end
	Game.LLMSetActiveMods(table.concat(mods, "\n"));
end
ReportActiveMods();

-- The game's "Processing turn for ..." banner (TurnProcessing.lua) skips human players, and a
-- seat is a human slot, so the banner stays blank during a seat's turn. This fills it the way
-- the game does for an AI civilization; the game hides it again when the human's turn starts.
local TURN_BANNER = "/InGame/WorldView/TurnProcessing";

local function BannerControl(id)
	return ContextPtr:LookUpControl(TURN_BANNER .. "/" .. id);
end

local function ShowSeatTurnBanner(iPlayerID)
	if PreGame.IsMultiplayerGame() then
		return;
	end
	local player = Players[iPlayerID];
	if player == nil or not player:IsHuman() or iPlayerID == Game.GetActivePlayer() or not player:IsTurnActive() then
		return;
	end
	local banner = ContextPtr:LookUpControl(TURN_BANNER);
	local anim = BannerControl("Anim");
	local title = BannerControl("TurnProcessingTitle") or BannerControl("Anim/TurnProcessingGrid/TurnProcessingTitle");
	local icon, iconBG, iconShadow = BannerControl("CivIcon"), BannerControl("CivIconBG"), BannerControl("CivIconShadow");
	if banner == nil or anim == nil or title == nil then
		print("LLMLog: the turn banner was not found at " .. TURN_BANNER);
		return;
	end
	banner:SetHide(false);
	local bMet = Teams[Players[Game.GetActivePlayer()]:GetTeam()]:IsHasMet(player:GetTeam());
	if bMet then
		title:SetText(Locale.ConvertTextKey("TXT_KEY_PROCESSING_TURN_FOR", Locale.ConvertTextKey(GameInfo.Civilizations[player:GetCivilizationType()].ShortDescription)));
	else
		title:SetText(Locale.ConvertTextKey("TXT_KEY_PROCESSING_TURN_FOR_UNMET_PLAYER", iPlayerID + 1));
	end
	if icon ~= nil and iconBG ~= nil and iconShadow ~= nil then
		if bMet then
			iconBG:SetHide(false);
			iconShadow:SetHide(false);
			CivIconHookup(iPlayerID, 64, icon, iconBG, iconShadow, false, true);
		else
			icon:SetTexture("CivSymbolsColor512.dds");
			icon:SetTextureOffsetVal(448 + 7, 128 + 7);
			icon:SetColor(Vector4(1.0, 1.0, 1.0, 1.0));
			icon:SetHide(false);
			iconBG:SetHide(true);
			iconShadow:SetHide(true);
		end
	end
	if anim:IsHidden() then
		anim:SetHide(false);
		anim:BranchResetAnimation();
	end
end
Events.AIProcessingStartedForPlayer.Add(ShowSeatTurnBanner);

local function Update(fDeltaTime)
	CollectAcknowledged();
	g_SinceIdentity = g_SinceIdentity + fDeltaTime;
	if g_SinceIdentity >= 1.0 then
		g_SinceIdentity = 0;
		UpdateIdentity();
	end
	if #g_PendingDismiss > 0 then
		DismissPending(fDeltaTime);
	end
	if Controls.Popup:IsHidden() then
		return;
	end
	g_SinceRefresh = g_SinceRefresh + fDeltaTime;
	if g_SinceRefresh >= 1.0 then
		g_SinceRefresh = 0;
		UpdateTeammates();
		Refresh();
		Game.LLMChatSeen();
	end
end

-- After a crash Bazaar turns the plugin off and every call to it raises. The first error closes the
-- window for good and is raised once more, so it still reaches Lua.log without repeating every frame.
ContextPtr:SetUpdate(function(fDeltaTime)
	local ok, err = pcall(Update, fDeltaTime);
	if not ok then
		g_Stopped = true;
		ContextPtr:ClearUpdate();
		Controls.Popup:SetHide(true);
		Controls.OpenButton:SetHide(true);
		error(err, 0);
	end
end);

ContextPtr:SetInputHandler(function(uiMsg, wParam, lParam)
	if Controls.Popup:IsHidden() then
		return false;
	end
	if uiMsg == KeyEvents.KeyDown and wParam == Keys.VK_ESCAPE then
		Close();
		return true;
	end
	-- Enter belongs to the chat box while the log is open: sent on from here, and kept from the
	-- game, whose Enter ends the turn.
	if wParam == Keys.VK_RETURN and (uiMsg == KeyEvents.KeyDown or uiMsg == KeyEvents.KeyUp) then
		if uiMsg == KeyEvents.KeyDown then
			SendChat(Controls.ChatEntry:GetText());
		end
		return true;
	end
	return false;
end);

-- Tile coordinates in the game's tile box above the minimap, so the human can name a tile to
-- the AI. Relies on the game filling the box on the same event first; the game's debug mode
-- already prints coordinates there.
local function PlotHelpControl(id)
	return ContextPtr:LookUpControl("/InGame/WorldView/PlotHelpText/" .. id);
end

local g_PlotHelpMissingReported = false;

local function AddTileCoordinates(hexX, hexY)
	local plot = Map.GetPlot(hexX, hexY);
	if plot == nil or OptionsManager:IsDebugMode() then
		return;
	end
	local box, label = PlotHelpControl("TextBox"), PlotHelpControl("Text");
	if box == nil or label == nil then
		if not g_PlotHelpMissingReported then
			print("LLMLog: the tile info box was not found; no coordinates will be shown");
			g_PlotHelpMissingReported = true;
		end
		return;
	end
	local coords = "(" .. plot:GetX() .. ", " .. plot:GetY() .. ")";
	local text = box:IsHidden() and "" or label:GetText();
	if string.sub(text, -string.len(coords)) == coords then
		return;
	end
	if text == "" then
		label:SetText(coords);
	else
		label:SetText(text .. "[NEWLINE]" .. coords);
	end
	box:SetHide(false);
	box:DoAutoSize();
	box:ReprocessAnchoring();
end
Events.SerialEventMouseOverHex.Add(AddTileCoordinates);

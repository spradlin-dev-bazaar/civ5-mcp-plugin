"""Checks the MCP server plugin by calling its tools in a fixed order, the way a model would, against the game in
saves/mcp_test.Civ5Save, and prints PASS or FAIL for each check. The same calls in the same order make the game's AI
answer the same way every run, so the checks expect exact results; a change to the calls changes what comes after.

    python tests/ingame/mcp_checks.py

run.py has Bazaar's checker load the save and then runs these checks; this file can also run on its own against that
save, freshly loaded and waiting in the game.
"""

import base64
import collections
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

from bazaar_check import game, tuner

URL = "http://127.0.0.1:7676/mcp"
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VERSION_FILE = os.path.join(REPO, "VERSION")
TOOLS = [
    "status",
    "wait_for_my_turn",
    "briefing",
    "overview",
    "units",
    "unit_options",
    "cities",
    "city_options",
    "map",
    "research_options",
    "policy_options",
    "game_settings",
    "rankings",
    "mods",
    "civs",
    "lookup",
    "move_unit",
    "unit_action",
    "attack",
    "city_attack",
    "promote_unit",
    "upgrade_unit",
    "set_production",
    "buy",
    "buy_tile",
    "set_city_focus",
    "city_citizens",
    "city_work_tile",
    "city_specialist",
    "set_research",
    "adopt_policy",
    "declare_war",
    "end_turn",
    "acknowledge_messages",
    "diplomacy",
    "respond_to_request",
    "deal_options",
    "propose_deal",
    "make_peace",
    "answer_deal",
    "demand",
    "discuss",
    "religion_options",
    "found_pantheon",
    "found_religion",
    "enhance_religion",
    "add_reformation_belief",
    "choose_great_person",
    "congress",
    "propose_resolution",
    "vote_resolution",
    "abstain",
    "spies",
    "move_spy",
    "stage_coup",
    "steal_tech",
    "city_state",
    "city_state_action",
    "choose_ideology",
    "switch_ideology",
    "archaeology",
    "great_works",
    "move_great_work",
    "trade_route_options",
    "establish_trade_route",
    "launch_spaceship",
    "advance_round",
    "city_task",
    "set_identity",
    "take_seat",
    "release_seat",
    "skip_human_turn",
    "testing_mode",
    "log",
    "chat",
    "economic_overview",
    "trade_routes",
    "military_overview",
    "religion_overview",
    "victory_progress",
    "map_image",
    "known_cities",
]

# The save's own ids: Denmark's cities and units at turn 150, and the units the scenario makes or takes.
COPENHAGEN, AARHUS, KAUPANG = 8192, 16385, 24578
GREAT_WRITER, WORKER, WORKER_2, CARAVAN, GREAT_PROPHET = 344074, 90119, 98312, 286732, 303107
BERSERKER_GAO, BERSERKER_NORTH, BERSERKER_EAST, BERSERKER_WEST = 237568, 139269, 262155, 229377
KNIGHT_EAST, SWORDSMAN, TREBUCHET_AARHUS, TREBUCHET_NEW = 204806, 106505, 294914, 352266
MUSKETMAN, MISSIONARY, CAPTURED_WORKER, TREBUCHET_FARM, KNIGHT_KAUPANG = 401426, 385039, 409600, 327684, 335885
DENMARK, INDIA, RUSSIA, POLAND = 2, 3, 4, 5
DELHI, VIJAYANAGARA, ROSTOV = 8192, 24578, 32771
ROSTOV_TILE, MOSCOW_TILE, MUSKETMAN_TILE = dict(x=54, y=37), dict(x=48, y=39), dict(x=37, y=27)
IDLE_UNITS = {
    BERSERKER_GAO: ("Berserker", "fortify"),
    TREBUCHET_FARM: ("Trebuchet", "sleep"),
    KNIGHT_EAST: ("Knight", "sleep"),
    KNIGHT_KAUPANG: ("Knight", "sleep"),
}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# The question window closes only through its own HideWindow, as its buttons do: closed from outside, it shows no
# question for the rest of the game.
GENERIC_POPUP = "GenericPopup"
POPUP = "Events.SerialEventGameMessagePopup({Type = ButtonPopupTypes.%s, %s})"
GREAT_WORK = "BUTTONPOPUP_GREAT_WORK_COMPLETED_ACTIVE_PLAYER"
GREAT_WORK_LOOKED_AT = POPUP % (GREAT_WORK, "Data1 = 0, Data2 = %d, Priority = PopupPriority.Current" % INDIA)
GREAT_WORK_RECEIVED = POPUP % (GREAT_WORK, "Data1 = 0, Data2 = %d" % INDIA)
CONGRESS_FOUNDED = POPUP % ("BUTTONPOPUP_LEAGUE_SPLASH", "Data1 = 0, Data2 = %d, Option1 = true" % RUSSIA)
CONGRESS_SPECIAL_SESSION = POPUP % ("BUTTONPOPUP_LEAGUE_SPLASH", "Data1 = 0, Data2 = %d, Option1 = false" % RUSSIA)
QUESTION_OPEN = "print(tostring(not ContextPtr:IsHidden()))"
KEEP_CAPTURED_WORKER = "Network.SendReturnCivilian(false, %d, %d) HideWindow()" % (RUSSIA, CAPTURED_WORKER)
CLOSE_QUESTION = "HideWindow()"
ROSTOV_AT_ONE_HP = (
    "local c = Map.GetPlot(%(x)d, %(y)d):GetPlotCity() c:SetDamage(c:GetMaxHitPoints() - 1)" % ROSTOV_TILE
)
PLACE_RUSSIAN_MUSKETMAN = "Players[%(russia)d]:InitUnit(GameInfoTypes.UNIT_MUSKETMAN, %(x)d, %(y)d)" % dict(
    MUSKETMAN_TILE, russia=RUSSIA
)
FREE_CHOICES = (
    "local p = Players[%d] p:ChangeNumFreeGreatPeople(1) p:SetNumFreeTechs(1) p:ChangeNumFreePolicies(1)"
    " p:SetHasPolicy(GameInfoTypes.POLICY_REFORMATION, true)" % DENMARK
)
# A capture from someone other than the city's founder, the case where the game offers liberation.
MOSCOW_TAKEN_FROM_POLAND = (
    "local p = Map.GetPlot(%(x)d, %(y)d)"
    " Players[%(poland)d]:AcquireCity(p:GetPlotCity(), true, false)"
    " Players[%(denmark)d]:AcquireCity(p:GetPlotCity(), true, false)"
    " print(p:GetPlotCity():GetID())" % dict(MOSCOW_TILE, poland=POLAND, denmark=DENMARK)
)

Result = collections.namedtuple("Result", "what passed found")


class Refused(Exception):
    pass


class StepStopped(Exception):
    """A check the rest of its step depends on failed; it has already been reported."""


class Mcp:
    """A client for the server's MCP endpoint: JSON-RPC over HTTP, one session."""

    def __init__(self):
        self.session = None
        self.next_id = 0

    def post(self, message, timeout):
        request = urllib.request.Request(URL, json.dumps(message).encode(), {"Content-Type": "application/json"})
        if self.session:
            request.add_header("Mcp-Session-Id", self.session)
        with urllib.request.urlopen(request, timeout=timeout) as response:
            self.session = response.headers.get("Mcp-Session-Id") or self.session
            return response.status, response.read()

    def rpc(self, method, params=None, timeout=120):
        self.next_id += 1
        _, body = self.post({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}, timeout)
        return json.loads(body)

    def notify(self, method):
        """Sends a notification, which has no id and gets no answer; returns the HTTP status and body."""
        return self.post({"jsonrpc": "2.0", "method": method}, 30)

    def connect(self, timeout=120):
        """The server opens once the game has loaded, so it is asked again until it answers."""
        deadline = time.time() + timeout
        while True:
            try:
                return self.rpc(
                    "initialize",
                    {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "mcp_checks"}},
                )["result"]
            except (urllib.error.URLError, ConnectionError, TimeoutError):
                if time.time() > deadline:
                    raise
                time.sleep(2)

    def call(self, tool, **args):
        """The tool's answer parsed from JSON, or with an image a list of its parts, the image as
        {"image": mimeType, "data": bytes}; raises Refused with the server's reason."""
        data = self.rpc("tools/call", {"name": tool, "arguments": args})
        if "error" in data:
            raise Refused(data["error"].get("message", str(data["error"])))
        result = data["result"]
        parts = []
        for part in result.get("content", []):
            if part.get("type") == "image":
                parts.append({"image": part.get("mimeType"), "data": base64.b64decode(part.get("data", ""))})
                continue
            try:
                parts.append(json.loads(part["text"]))
            except ValueError:
                parts.append(part["text"])
        if result.get("isError"):
            raise Refused(" ".join(str(p) for p in parts))
        return parts[0] if len(parts) == 1 else parts


class Checks:
    def __init__(self, mcp):
        self.mcp = mcp
        self.results = []
        self.for_later = {}

    def check(self, what, passed, found=""):
        self.results.append(Result(what, bool(passed), found))
        print("  %s  %s" % ("PASS" if passed else "FAIL", what), flush=True)
        if not passed and found:
            print("        " + str(found), flush=True)

    def equal(self, what, actual, expected):
        self.check(what, actual == expected, "it gave %r, not %r" % (actual, expected))

    def answers(self, what, tool, **args):
        """Calls the tool and returns its answer; the check passes when the answer is JSON. A refusal or any other
        answer fails the check and stops the step, which cannot use it."""
        try:
            answer = self.mcp.call(tool, **args)
        except Refused as e:
            self.check(what, False, "refused: %s" % e)
            raise StepStopped()
        self.check(what, isinstance(answer, (dict, list)), "it gave %r" % str(answer)[:200])
        if not isinstance(answer, (dict, list)):
            raise StepStopped()
        return answer

    def refuses(self, what, tool, pattern, **args):
        """Checks that the tool refuses for the reason the pattern matches. A call that goes through instead has changed
        the game, so it stops the step."""
        try:
            answer = self.mcp.call(tool, **args)
        except Refused as e:
            self.check(what, re.search(pattern, str(e)), "refused with: %s" % e)
            return
        self.check(what, False, "it was not refused: %s" % json.dumps(answer)[:200])
        raise StepStopped()


def released_version():
    with open(VERSION_FILE, encoding="utf-8") as f:
        return re.search(r"^release (\S+)$", f.read(), re.M).group(1)


def without_version(name):
    return re.sub(r" \(v \d+\)$", "", name)


def work_titles(works):
    """The titles in each city's great work slots, by city name, in the order great_works lists them."""
    return {
        city["city"]: [slot["work"] for building in city["buildings"] for slot in building["slots"]]
        for city in works["cities"]
    }


def wait_for_turn(c, turn, tries=12):
    """Waits until it is the session's turn; returns the status, which may be an earlier turn that came back. turn is
    only for the message if it does not come."""
    for _ in range(tries):
        status = c.mcp.call("wait_for_my_turn", timeout_seconds=50)
        if status.get("is_my_turn"):
            return status
    raise RuntimeError("turn %d did not come within %d waits; the last status: %s" % (turn, tries, status))


def wait_for_seat_turn(c, turn, tries=12):
    """Waits for a seat's turn while nobody plays the screen: a unit that finishes a queued move with moves to spare
    reopens the idle human's turn, so it is skipped again."""
    refused = None
    for _ in range(tries):
        status = c.mcp.call("wait_for_my_turn", timeout_seconds=20)
        if status.get("is_my_turn"):
            return status
        try:
            c.mcp.call("skip_human_turn")
        except Refused as e:
            refused = e
    raise RuntimeError(
        "the seat's turn %d did not come within %d waits; skip_human_turn last refused with %s; the last status: %s"
        % (turn, tries, refused, status)
    )


def end_turn_and_wait(c, turn, summary=None, tries=5):
    """Ends the turn and waits for the next, as a model does: when the turn comes back early, because a unit finished a
    queued move with moves to spare, the idle units skip and the turn ends again. Returns the next turn's status and
    the units that brought the turn back, in order."""
    reopened_by = []
    for _ in range(tries):
        ended = c.mcp.call("end_turn", summary=summary or "mcp_checks: turn %d." % turn)
        c.equal("end_turn ends turn %d" % turn, ended.get("ended_turn"), turn)
        status = wait_for_turn(c, turn + 1)
        if status["turn"] != turn:
            return status, reopened_by
        c.mcp.call("acknowledge_messages")
        for unit in c.mcp.call("units")["units"]:
            if unit["needs_orders"]:
                reopened_by.append("%s %d at (%d,%d)" % (unit["name"], unit["id"], unit["x"], unit["y"]))
                c.mcp.call("unit_action", unit_id=unit["id"], action="skip")
    raise RuntimeError("turn %d kept coming back: %s" % (turn, reopened_by))


def answer_requests(c, briefing, expected, choice=None):
    """Answers every request from another leader with choice, or else its first choice, as the checks expect them to
    arrive."""
    requests = briefing["diplomacy_requests"]
    c.equal("the requests from other leaders", [(r["from"], r["kind"]) for r in requests], expected)
    for r in requests:
        answer = choice or r["choices"].split(", ")[0]
        c.answers(
            "respond_to_request: %s to %s's %s" % (answer, r["from"], r["kind"]),
            "respond_to_request",
            id=r["id"],
            choice=answer,
        )


def notices_of_turn(briefing, turn):
    return [(e["summary"], e["text"]) for e in briefing["events"] if e["turn"] == turn]


def wait_for_unread_message(c, text, seconds=20):
    """The unread game message containing text, or None, and the unread messages: the game reports it a few frames
    after the event."""
    deadline = time.time() + seconds
    while True:
        unread = [m["text"] for m in c.mcp.call("briefing")["game_messages"] if m.get("unread")]
        said = next((message for message in unread if text in message), None)
        if said or time.time() >= deadline:
            return said, unread
        time.sleep(1)


def turn_begins(c, turn):
    """The briefing that opens a turn, checked to be for that turn."""
    briefing = c.answers("briefing on turn %d" % turn, "briefing")
    c.equal("the briefing is for turn %d" % turn, briefing["turn"], turn)
    return briefing


def hear_and_acknowledge(c, what, text):
    """Waits for the unread game message containing text, then acknowledges every unread message."""
    said, unread = wait_for_unread_message(c, text)
    c.check(what, said, "the unread game messages: %s" % ("; ".join(unread) or "none"))
    c.answers("acknowledge: " + what, "acknowledge_messages")


def game_lua(code, state=game.IN_GAME):
    port = tuner.Tuner()
    port.connect(10)
    try:
        return port.run(state, code)
    finally:
        port.close()


def second_model(c):
    other = Checks(Mcp())
    other.results = c.results
    other.mcp.connect()
    other.mcp.notify("notifications/initialized")
    named = other.answers("set_identity for a second model", "set_identity", provider="Test", model="Seat", version="2")
    other.equal("the second model is named", named.get("playing_as"), "Test Seat 2")
    return other


def unit_of_type(c, unit_type, needs_orders=False):
    for unit in c.mcp.call("units")["units"]:
        if unit["type"] == unit_type and (unit["needs_orders"] or not needs_orders):
            return unit
    raise RuntimeError("no unit of type %s%s" % (unit_type, " needs orders" if needs_orders else ""))


def event_summaries(briefing):
    return [e["summary"] for e in briefing["events"]]


def connecting(c):
    init = c.mcp.connect()
    c.equal("the server names itself", init["serverInfo"]["name"], "civ5-mcp-plugin")
    c.equal("the server reports VERSION's release", init["serverInfo"]["version"], released_version())
    status, body = c.mcp.notify("notifications/initialized")
    c.equal("notifications/initialized gets 202 and no body", (status, body), (202, b""))
    names = [tool["name"] for tool in c.mcp.rpc("tools/list")["result"]["tools"]]
    missing, extra = sorted(set(TOOLS) - set(names)), sorted(set(names) - set(TOOLS))
    c.check(
        "tools/list lists every expected tool",
        not missing and not extra,
        "missing %s, not expected %s" % (missing, extra),
    )


def naming(c):
    c.refuses("an action before set_identity is refused", "set_research", "name yourself first", tech="TECH_GUNPOWDER")
    named = c.answers("set_identity", "set_identity", provider="Test", model="Checks", version="1")
    c.equal("set_identity names the session", named.get("playing_as"), "Test Checks 1")


def turn_150_status(c):
    s = c.answers("status", "status")
    c.equal("the save plays Denmark", s["civilization"], "Denmark")
    c.equal("Denmark is player 2", s["player_id"], 2)
    c.equal("it is turn 150", s["turn"], 150)
    c.equal("it is Denmark's turn", s["is_my_turn"], True)
    c.equal("India is the teammate a model could play", s["teammate_civs_you_could_play"], ["India"])
    c.check(
        "a city with nothing to build blocks the turn",
        "production: a city needs a production choice" in s["end_turn_blocking"],
        s["end_turn_blocking"],
    )


def spies_religion_and_neighbours(c):
    spies = c.answers("spies", "spies")
    c.equal(
        "spies names the spy in Gao",
        [(s["name"], s["city"], s["city_owner"]) for s in spies["spies"]],
        [("Helle", "Gao", "Songhai")],
    )
    religion = c.answers("religion_options", "religion_options")["my_religion"]
    c.equal(
        "religion_options: Denmark founded Catholicism in Copenhagen",
        (religion["name"], religion["founder"], religion["holy_city"]),
        ("Catholicism", "Denmark", "Copenhagen"),
    )
    c.equal(
        "religion_options: a belief's effect without its icons",
        religion["beliefs"][0]["effect"],
        "+1 Culture and +1 Faith for each Gold and Silver",
    )
    states = {s["civilization"]: s for s in c.answers("city_state", "city_state")["city_states"]}
    c.equal(
        "city_state lists the city-states met",
        sorted(states),
        ["Bogota", "Mogadishu", "Panama City", "Samarkand", "Tyre", "Zurich"],
    )
    c.equal(
        "Tyre is Denmark's ally", (states["Tyre"]["status"], states["Tyre"].get("their_ally")), ("allies", "Denmark")
    )
    diplomacy = c.answers("diplomacy", "diplomacy")
    c.equal("diplomacy: India is the teammate", diplomacy["your_teammates"], ["India"])
    c.equal("diplomacy: the number of deals in force", len(diplomacy["current_deals"]), 6)
    routes = c.answers("trade_routes", "trade_routes")
    c.equal("trade_routes: routes in use and possible", (len(routes["your_routes"]), routes["routes_possible"]), (2, 6))


def works_and_the_empire(c):
    works = c.answers("great_works", "great_works")
    c.equal(
        "great_works: the works in each city's slots",
        work_titles(works),
        {"Copenhagen": ["Henry VIII", "Chu Ci (Songs of the South)"], "Aarhus": ["empty"]},
    )
    c.refuses("congress before the World Congress is founded", "congress", "has not been founded")
    overview = c.answers("overview", "overview")
    c.equal("overview: cities and era", (overview["num_cities"], overview["era"]), (3, "Renaissance Era"))
    civs = c.answers("civs", "civs")
    c.equal(
        "civs lists the other civilizations",
        sorted(x["civilization"] for x in civs["civilizations"]),
        ["India", "Poland", "Russia", "Songhai", "Spain"],
    )
    settings = c.answers("game_settings", "game_settings")
    c.equal(
        "game_settings: map size, map type and pace",
        (settings["map_size"], settings["map_type"], settings["game_pace"]),
        ("Small", "ContinentsPlus", "Quick"),
    )
    mods = c.answers("mods", "mods")
    c.equal(
        "mods lists Bazaar, its bootstrap and this plugin",
        [without_version(m["name"]) for m in mods["mods"]],
        ["Bazaar Multi-DLL Mod Loader", "Bazaar - VMC Bootstrap (required)", "MCP Server for Bazaar"],
    )
    descriptions = [m["description"] for m in mods["mods"]]
    c.check(
        "mods gives whole descriptions without markup",
        all("[" not in d and not d.endswith("...") for d in descriptions),
        descriptions,
    )
    cities = c.answers("cities", "cities")["cities"]
    c.check(
        "cities: each city says when it grows or why it does not",
        all(("turns_to_grow" in city) != ("not_growing" in city) for city in cities),
        [(city["name"], city.get("turns_to_grow"), city.get("not_growing")) for city in cities],
    )


def research_and_policies(c):
    research = c.answers("research_options", "research_options")
    c.equal("research_options: the technology being researched", research["research"]["type"], "TECH_GUNPOWDER")
    policies = c.answers("policy_options", "policy_options")
    c.equal(
        "policy_options: culture, the next policy's cost, and whether one can be adopted",
        (policies["culture"], policies["next_policy_cost"], policies["can_adopt_now"]),
        (537, 690, False),
    )
    c.equal("archaeology: no dig waiting", c.answers("archaeology", "archaeology")["digs_waiting"], 0)
    lookup = c.answers("lookup TECH_GUNPOWDER", "lookup", type="TECH_GUNPOWDER")
    c.check("lookup names the technology", "Gunpowder" in json.dumps(lookup), json.dumps(lookup)[:200])
    unlocks = c.answers("lookup TECH_MASONRY", "lookup", type="TECH_MASONRY")["tech"]["unlocks"]
    builds = [u for u in unlocks if u.startswith("worker build: ")]
    c.check(
        "lookup names a worker build in words, without a text key or markup",
        any("(BUILD_QUARRY)" in b for b in builds) and not any("TXT_KEY" in b or "[" in b for b in builds),
        unlocks,
    )
    parts = c.answers("map_image", "map_image")
    images = [p for p in parts if isinstance(p, dict) and "image" in p]
    c.check(
        "map_image draws a PNG",
        len(images) == 1 and images[0]["image"] == "image/png" and images[0]["data"].startswith(PNG_SIGNATURE),
        [(p["image"], len(p["data"])) for p in images],
    )
    for tool in (
        "briefing",
        "map",
        "rankings",
        "known_cities",
        "economic_overview",
        "military_overview",
        "religion_overview",
        "victory_progress",
    ):
        c.answers(tool, tool)


def every_unit_city_and_civ(c):
    units = c.answers("units", "units")["units"]
    c.equal("units lists Denmark's units", len(units), 15)
    for unit in units:
        c.answers("unit_options for %s %d" % (unit["name"], unit["id"]), "unit_options", unit_id=unit["id"])
    for city in (COPENHAGEN, AARHUS, KAUPANG):
        c.answers("city_options for city %d" % city, "city_options", city_id=city)
        c.answers("city_citizens for city %d" % city, "city_citizens", city_id=city)
    for civ in ("Spain", "Songhai", "India", "Russia", "Poland"):
        c.answers("deal_options with %s" % civ, "deal_options", civ=civ)


def deal_preview(c):
    preview = c.answers(
        "propose_deal preview: Salt for Russia's Marble",
        "propose_deal",
        civ="Russia",
        preview_only=True,
        give=[{"item": "resource", "resource": "Salt", "amount": 1}],
        get=[{"item": "resource", "resource": "Marble", "amount": 1}],
    )
    asked = sorted(i["resource"] for i in preview["what_would_work"]["items"] if i["given_by"] == "Denmark")
    c.equal(
        "propose_deal preview: whether Russia accepts, and what Denmark would have to give",
        (preview["acceptable"], asked),
        (False, ["Citrus", "Cotton", "Salt"]),
    )


def refusals(c):
    c.refuses("unit_options of a unit that is not there", "unit_options", "no unit with that id", unit_id=99999)
    c.refuses("make_peace when not at war", "make_peace", "not at war", civ="Russia", preview_only=True)
    c.refuses("declare_war on a teammate", "declare_war", "you cannot declare war on them right now", civ="India")
    c.refuses("chat from the civilization on screen", "chat", "seat with a human teammate", text="hello")
    c.refuses(
        "end_turn while a city builds nothing",
        "end_turn",
        "production: a city needs a production choice",
        summary="refused",
    )
    c.refuses(
        "set_production of a unit Denmark cannot train yet",
        "set_production",
        "cannot train UNIT_MUSKETMAN",
        city_id=COPENHAGEN,
        item="UNIT_MUSKETMAN",
    )
    c.refuses(
        "trade_route_options for a caravan already on a route",
        "trade_route_options",
        "cannot start a trade route here",
        unit_id=CARAVAN,
    )
    c.refuses(
        "gift_gold beyond the treasury",
        "city_state_action",
        "between 1 and your gold",
        civ="Tyre",
        action="gift_gold",
        amount=250,
    )
    c.refuses("an unknown tool", "no_such_tool", "unknown tool")


def turn_150_actions(c):
    production = c.answers(
        "set_production Harbor in Copenhagen", "set_production", city_id=COPENHAGEN, item="BUILDING_HARBOR"
    )
    c.equal("Copenhagen builds a Harbor", production["city"]["production"]["name"], "Harbor")
    queued = c.answers(
        "set_production appends a Market", "set_production", city_id=COPENHAGEN, item="BUILDING_MARKET", append=True
    )
    c.equal("the Market waits behind it", queued["city"]["production_queue_after_current"], ["Market"])
    c.answers("the Great Writer writes a great work", "unit_action", unit_id=GREAT_WRITER, action="create_great_work")
    titles = work_titles(c.answers("great_works", "great_works"))
    c.equal(
        "Copenhagen's works stay where they were",
        titles.get("Copenhagen"),
        ["Henry VIII", "Chu Ci (Songs of the South)"],
    )
    c.check("the new work fills Aarhus's empty slot", titles.get("Aarhus") not in (None, ["empty"]), titles)
    said, unread = wait_for_unread_message(c, "Created the great work ")
    c.check(
        "the Great Work pop-up reaches the model as a game message naming the work and its city",
        said and (titles.get("Aarhus") or [""])[0] in said and said.endswith(", now in Aarhus"),
        "the unread game messages: %s" % ("; ".join(unread) or "none"),
    )
    c.answers("acknowledge the Great Work", "acknowledge_messages")
    said, _ = wait_for_unread_message(c, "Created the great work ", seconds=0)
    c.equal("acknowledge_messages marks the Great Work message read", said, None)
    c.equal("the treasury before buying a tile", c.answers("overview", "overview")["gold"], 67)
    c.answers("buy_tile (39,23) for Copenhagen", "buy_tile", city_id=COPENHAGEN, x=39, y=23)
    c.equal("buy_tile takes the tile's price from the treasury", c.answers("overview", "overview")["gold"], 37)
    c.answers("the worker builds a road", "unit_action", unit_id=WORKER, action="build", build="BUILD_ROAD")
    for unit, (name, action) in IDLE_UNITS.items():
        c.answers("%s %d: %s" % (name, unit, action), "unit_action", unit_id=unit, action=action)
    c.answers("log a line", "log", text="mcp_checks: turn 150 orders given")
    last = c.answers("the last briefing of turn 150", "briefing")
    c.for_later["turn 150 notices reported"] = notices_of_turn(last, 150)
    status, reopened_by = end_turn_and_wait(c, 150, "mcp_checks: Harbor, a great work, a tile and a road.")
    c.equal("Denmark's turn 151 comes", (status["civilization"], status["turn"]), ("Denmark", 151))
    c.equal(
        "units that finished their moves with moves left reopened turn 150",
        reopened_by,
        ["Berserker 262155 at (47,28)"],
    )


def turn_151(c):
    briefing = c.answers("briefing on turn 151", "briefing")
    c.equal("the briefing is for turn 151", briefing["turn"], 151)
    since_150 = c.answers("briefing since turn 150", "briefing", since_turn=150)
    reported = c.for_later["turn 150 notices reported"]
    c.check("turn 150's last briefing reported notices to leave out", reported, reported)
    c.equal(
        "the briefing shows the turn 150 notices that came after turn 150's last briefing, and only those",
        notices_of_turn(briefing, 150),
        [n for n in notices_of_turn(since_150, 150) if n not in reported],
    )
    c.answers("acknowledge_messages", "acknowledge_messages")
    answer_requests(c, briefing, [("Songhai", "deal_offer")])
    reply = c.answers("discuss: ask Russia for friendship", "discuss", civ="Russia", topic="ask_friendship")
    c.check(
        "discuss gives Russia's reply, without markup",
        reply.get("their_reply") and "[" not in reply["their_reply"],
        reply,
    )
    hear_and_acknowledge(c, "Russia's leader screen reaches the model as a game message", "Russia's leader says")


def seats(c):
    taken = c.answers("take_seat India", "take_seat", civ="India")
    c.equal("the session now plays India", taken["playing"], "India")
    c.answers("chat to the teammate on screen", "chat", text="mcp_checks: India here.")
    c.answers("testing_mode on", "testing_mode", enabled=True)
    c.answers("skip_human_turn ends Denmark's turn", "skip_human_turn")
    status = wait_for_seat_turn(c, 151)
    c.equal("India's turn 151 comes", (status["civilization"], status["turn"]), ("India", 151))
    briefing = c.answers("briefing for India", "briefing")
    c.answers("acknowledge_messages for India", "acknowledge_messages")
    answer_requests(c, briefing, [])
    c.answers("end_turn for India", "end_turn", summary="mcp_checks: India passes.", force=True)
    c.answers("release_seat", "release_seat")
    c.answers("take_seat human", "take_seat", civ="human")
    c.answers("testing_mode off", "testing_mode", enabled=False)
    status = wait_for_turn(c, 152)
    c.equal("Denmark's turn 152 comes", (status["civilization"], status["turn"]), ("Denmark", 152))


def turn_152(c):
    turn_begins(c, 152)
    c.answers("set_production Caravan in Aarhus", "set_production", city_id=AARHUS, item="UNIT_CARAVAN")
    queued = c.answers(
        "set_production appends a University", "set_production", city_id=AARHUS, item="BUILDING_UNIVERSITY", append=True
    )
    c.equal(
        "Aarhus builds the Caravan, then the University",
        (queued["city"]["production"]["name"], queued["city"]["production_queue_after_current"]),
        ("Caravan", ["University"]),
    )
    sent = c.answers("move_unit: the Great Prophet heads home", "move_unit", unit_id=GREAT_PROPHET, x=39, y=26)
    c.equal(
        "move_unit: a multi-turn move that has begun", (sent["estimated_turns_total"], sent["now_at"]), (7, "(42,35)")
    )
    work = c.answers(
        "the worker builds a trading post", "unit_action", unit_id=WORKER, action="build", build="BUILD_TRADING_POST"
    )
    c.equal(
        "the worker's activity names the build without markup",
        work["unit"]["activity"],
        "working: Construct a Trading Post",
    )
    automated = c.answers(
        "automate_work hands a worker to the game", "unit_action", unit_id=WORKER_2, action="automate_work"
    )
    c.check("automate_work warns what the game's AI may do", "warning" in automated, automated)
    c.refuses(
        "sleep for a trebuchet sharing its city with another",
        "unit_action",
        "same kind shares this tile",
        unit_id=TREBUCHET_AARHUS,
        action="sleep",
    )
    for unit in (BERSERKER_NORTH, SWORDSMAN, BERSERKER_EAST):
        c.answers("unit %d fortifies" % unit, "unit_action", unit_id=unit, action="fortify")
    c.answers("the knight heads east", "move_unit", unit_id=KNIGHT_EAST, x=56, y=31)
    c.answers("the berserker heads east", "move_unit", unit_id=BERSERKER_EAST, x=56, y=33)
    c.answers("the swordsman heads east", "move_unit", unit_id=SWORDSMAN, x=54, y=31)
    moved = c.answers("the new trebuchet moves to Copenhagen", "move_unit", unit_id=TREBUCHET_NEW, x=39, y=26)
    c.equal("move_unit: arrived this turn", moved["arrived"], True)
    c.answers("the Aarhus trebuchet sleeps", "unit_action", unit_id=TREBUCHET_AARHUS, action="sleep")
    c.answers("the moved trebuchet sleeps", "unit_action", unit_id=TREBUCHET_NEW, action="sleep")
    spy = c.answers("move_spy: Helle to Zurich", "move_spy", spy_index=0, x=34, y=38)
    c.equal("move_spy: Helle travels to Zurich", spy["travelling_to"], "Zurich")
    citizens = c.answers(
        "city_specialist: a scientist in Copenhagen",
        "city_specialist",
        city_id=COPENHAGEN,
        building="BUILDING_UNIVERSITY",
    )
    scientists = [s["count"] for s in citizens["citizens"]["specialists"] if s["type"] == "BUILDING_UNIVERSITY"]
    c.equal("city_specialist: Copenhagen has one scientist", scientists, [1])
    locked = c.answers("city_work_tile: lock the cotton", "city_work_tile", city_id=COPENHAGEN, x=38, y=24)
    c.equal("city_work_tile locks the tile", (locked["locked"], locked["now_worked"]), (True, True))
    c.refuses(
        "set_city_focus with an unknown focus",
        "set_city_focus",
        "use one of default, food",
        city_id=KAUPANG,
        focus="nonsense",
    )
    focus = c.answers("set_city_focus food in Kaupang", "set_city_focus", city_id=KAUPANG, focus="food")
    c.equal("Kaupang focuses on food", focus["city"]["focus"], "food")
    c.answers(
        "move_great_work swaps the two writings",
        "move_great_work",
        from_city_id=COPENHAGEN,
        from_building="BUILDING_NATIONAL_EPIC",
        from_slot=0,
        to_city_id=AARHUS,
        to_building="BUILDING_AMPHITHEATER",
        to_slot=0,
    )
    titles = work_titles(c.answers("great_works after the swap", "great_works"))
    c.equal(
        "the writings changed places",
        (titles["Copenhagen"][1], titles["Aarhus"]),
        ("Adventures of Huckleberry Finn", ["Chu Ci (Songs of the South)"]),
    )
    luxuries = [{"item": "resource", "resource": r, "amount": 1} for r in ("Cotton", "Salt", "Citrus")]
    marble = [{"item": "resource", "resource": "Marble", "amount": 1}]
    preview = c.answers(
        "propose_deal preview: Cotton for Spain's Marble",
        "propose_deal",
        civ="Spain",
        preview_only=True,
        give=luxuries[:1],
        get=marble,
    )
    c.equal(
        "Spain wants Salt and Citrus as well",
        (preview["acceptable"], len(preview["what_would_work"]["items"])),
        (False, 4),
    )
    deal = c.answers(
        "propose_deal: three luxuries for Spain's Marble", "propose_deal", civ="Spain", give=luxuries, get=marble
    )
    c.equal("Spain accepts", deal["acceptable"], True)
    hear_and_acknowledge(c, "Spain's leader answers the deal", "Spain's leader says: Very well.")
    pledged = c.answers(
        "pledge_protection to Samarkand", "city_state_action", civ="Samarkand", action="pledge_protection"
    )
    c.equal("Denmark protects Samarkand", pledged["city_state"]["protected_by_you"], True)
    end_turn_and_wait(c, 152)


def turn_153(c):
    briefing = turn_begins(c, 153)
    c.check(
        "Spain's Marble completes three city-state quests",
        all(
            "Marble Connected for %s!" % s in event_summaries(briefing)
            for s in ("Samarkand", "Panama City", "Mogadishu")
        ),
        event_summaries(briefing),
    )
    texts = [e["text"] for e in briefing["events"]]
    c.check(
        "notification texts lose their markup, icons leave a space, and blank lines no repeated '; '",
        all("[" not in t and "; ;" not in t for t in texts) and any("(+2 in Capital," in t for t in texts),
        texts,
    )
    answer_requests(c, briefing, [("Spain", "deal_offer")], "accept")
    adopted = c.answers("adopt_policy Artistic Genius", "adopt_policy", policy="POLICY_ARTISTIC_GENIUS")
    c.equal("the policy is adopted", adopted["result"], "policy adopted")
    artist = unit_of_type(c, "UNIT_ARTIST")
    c.answers("the Great Artist starts a Golden Age", "unit_action", unit_id=artist["id"], action="golden_age")
    economy = c.answers("briefing in the Golden Age", "briefing")["economy"]
    c.check("the briefing counts the Golden Age's turns", economy.get("golden_age_turns_left", 0) > 0, economy)
    end_turn_and_wait(c, 153)


def turn_154(c):
    briefing = turn_begins(c, 154)
    caravan = unit_of_type(c, "UNIT_CARAVAN", needs_orders=True)
    options = c.answers("trade_route_options for the new caravan", "trade_route_options", unit_id=caravan["id"])
    c.check(
        "trade_route_options offers Gao",
        any(d["city"] == "Gao" and d["type"] == "international" for d in options["destinations"]),
        options["destinations"],
    )
    route = c.answers(
        "establish_trade_route to Gao", "establish_trade_route", unit_id=caravan["id"], x=34, y=11, type="international"
    )
    c.equal("the route goes to Gao", route["route_to"], "Gao")
    answer_requests(c, briefing, [("Spain", "deal_offer")], "decline")
    c.answers("set_production Monument in Kaupang", "set_production", city_id=KAUPANG, item="BUILDING_MONUMENT")
    end_turn_and_wait(c, 154)


def turn_155(c):
    turn_begins(c, 155)
    c.refuses(
        "buy into a city tile another unit of that kind holds",
        "buy",
        "already stands on the city tile",
        city_id=AARHUS,
        item="UNIT_GERMAN_LANDSKNECHT",
    )
    bought = c.answers("buy a Missionary with faith", "buy", city_id=KAUPANG, item="UNIT_MISSIONARY", with_faith=True)
    c.equal("the Missionary's price in faith", bought["faith_spent"], 140)
    c.refuses("fortify a knight", "unit_action", "cannot fortify; use sleep", unit_id=KNIGHT_EAST, action="fortify")
    c.answers("the worker sleeps", "unit_action", unit_id=WORKER, action="sleep")
    c.answers("the knight sleeps", "unit_action", unit_id=KNIGHT_EAST, action="sleep")
    c.answers("the trebuchet leaves Aarhus", "move_unit", unit_id=TREBUCHET_AARHUS, x=36, y=24)
    bought = c.answers("buy a Landsknecht with gold", "buy", city_id=AARHUS, item="UNIT_GERMAN_LANDSKNECHT")
    c.equal("the Landsknecht's price in gold", bought["gold_spent"], 120)
    c.answers("the trebuchet sleeps", "unit_action", unit_id=TREBUCHET_AARHUS, action="sleep")
    landsknecht = unit_of_type(c, "UNIT_GERMAN_LANDSKNECHT")
    c.answers("the Landsknecht fortifies", "unit_action", unit_id=landsknecht["id"], action="fortify")
    _, reopened_by = end_turn_and_wait(c, 155)
    c.equal("the swordsman's arrival reopened turn 155", reopened_by, ["Swordsman %d at (54,31)" % SWORDSMAN])


def turn_156(c):
    briefing = turn_begins(c, 156)
    hear_and_acknowledge(c, "Gunpowder's pop-up reaches the model", "Researched Gunpowder")
    answer_requests(c, briefing, [("Russia", "friendship_offer")], "accept")
    hear_and_acknowledge(c, "Russia's leader answers the friendship", "Russia's leader says: Excellent.")
    upgraded = c.answers("upgrade_unit: a Berserker to a Musketman", "upgrade_unit", unit_id=BERSERKER_GAO)
    c.equal(
        "the Berserker became a Musketman, for gold",
        (upgraded["unit"]["type"], upgraded["unit"]["id"], upgraded["gold_spent"]),
        ("UNIT_MUSKETMAN", MUSKETMAN, 50),
    )
    research = c.answers("set_research Printing Press", "set_research", tech="TECH_PRINTING_PRESS")
    c.equal("Printing Press is researched", research["research"]["current"], "Printing Press")
    c.refuses(
        "attack from too far away",
        "attack",
        "need the unit adjacent",
        unit_id=BERSERKER_EAST,
        x=52,
        y=34,
        preview_only=True,
    )
    c.answers("the berserker lands near the barbarians", "move_unit", unit_id=BERSERKER_EAST, x=53, y=33)
    c.answers("the knight lands", "move_unit", unit_id=KNIGHT_EAST, x=54, y=32)
    c.answers("the swordsman fortifies", "unit_action", unit_id=SWORDSMAN, action="fortify")
    c.refuses(
        "move_unit into a city-state's city",
        "move_unit",
        "no path",
        unit_id=MISSIONARY,
        x=30,
        y=36,
        preview_only=True,
    )
    c.answers("set_production Shrine in Kaupang", "set_production", city_id=KAUPANG, item="BUILDING_SHRINE")
    c.answers("the missionary heads for Tyre", "move_unit", unit_id=MISSIONARY, x=31, y=35)
    end_turn_and_wait(c, 156)


def turn_157(c):
    briefing = turn_begins(c, 157)
    answer_requests(c, briefing, [("Russia", "request_for_help")], "decline")
    hear_and_acknowledge(c, "Russia's leader answers the refusal", "Russia's leader says: Perhaps another time then.")
    c.answers("the berserker moves next to the barbarian", "move_unit", unit_id=BERSERKER_EAST, x=53, y=34)
    preview = c.answers(
        "attack preview on the barbarian swordsman", "attack", unit_id=BERSERKER_EAST, x=52, y=35, preview_only=True
    )
    c.equal("the preview gives the game's estimate", preview["estimate"]["expected_damage_to_target"], 52)
    hit = c.answers("the berserker attacks", "attack", unit_id=BERSERKER_EAST, x=52, y=35)
    c.equal("the attack's outcome", (hit["target_hp"], hit["your_hp"]), (48, 82))
    c.answers("the knight moves next to the barbarian", "move_unit", unit_id=KNIGHT_EAST, x=53, y=35)
    hit = c.answers("the knight attacks", "attack", unit_id=KNIGHT_EAST, x=52, y=35)
    c.equal("the knight's attack", (hit["target_hp"], hit["your_hp"]), (3, 87))
    c.answers("the musketman fortifies", "unit_action", unit_id=MUSKETMAN, action="fortify")
    c.answers("set_production Bank in Copenhagen", "set_production", city_id=COPENHAGEN, item="BUILDING_BANK")
    c.answers("end_turn 157", "end_turn", summary="mcp_checks: turn 157.")
    status = wait_for_turn(c, 157)
    c.equal("the great prophet's early arrival reopens turn 157", status["turn"], 157)
    c.answers("acknowledge_messages", "acknowledge_messages")
    prophet = c.answers("unit_options for the great prophet", "unit_options", unit_id=GREAT_PROPHET)
    c.equal("the prophet can only spread the religion", prophet["actions"], ["sleep", "skip", "spread_religion"])
    c.refuses(
        "enhance_religion with no enhancement waiting",
        "enhance_religion",
        "no enhancement is waiting",
        beliefs=["BELIEF_FEED_WORLD", "BELIEF_RELIQUARY"],
    )
    c.answers("the prophet sleeps", "unit_action", unit_id=GREAT_PROPHET, action="sleep")
    c.refuses(
        "move_unit onto terrain it cannot enter",
        "move_unit",
        "cannot enter that terrain",
        unit_id=MISSIONARY,
        x=31,
        y=36,
    )
    c.answers("the missionary heads beside Tyre", "move_unit", unit_id=MISSIONARY, x=30, y=35)
    end_turn_and_wait(c, 157)


def turn_158(c):
    turn_begins(c, 158)
    hear_and_acknowledge(c, "meeting a city-state reaches the model", "Met the city-state of M'banza-Kongo")
    hit = c.answers("the knight finishes the barbarian", "attack", unit_id=KNIGHT_EAST, x=53, y=36)
    c.equal("the barbarian swordsman is destroyed", (hit.get("target_destroyed"), hit["your_hp"]), (True, 72))
    c.answers("the berserker moves next to the camp", "move_unit", unit_id=BERSERKER_EAST, x=52, y=35)
    hit = c.answers("the berserker attacks the camp", "attack", unit_id=BERSERKER_EAST, x=51, y=35)
    c.equal("the camp's pikeman is hurt", (hit["target_hp"], hit["your_hp"]), (62, 58))
    c.answers("the knight sleeps", "unit_action", unit_id=KNIGHT_EAST, action="sleep")
    c.answers("the swordsman heads for the camp", "move_unit", unit_id=SWORDSMAN, x=52, y=34)
    gift = c.answers("gift_gold to Tyre", "city_state_action", civ="Tyre", action="gift_gold", amount=50)
    c.equal("Tyre is Denmark's ally again", gift["city_state"]["status"], "allies")
    end_turn_and_wait(c, 158)


def turn_159(c):
    briefing = turn_begins(c, 159)
    c.check(
        "the briefing says the knight can be promoted",
        "promotion: Knight (id %d) can be promoted (unit_options, promote_unit)" % KNIGHT_EAST
        in briefing["decisions_pending"],
        briefing["decisions_pending"],
    )
    options = c.answers("unit_options for the knight", "unit_options", unit_id=KNIGHT_EAST)
    c.check(
        "unit_options lists Shock II",
        "PROMOTION_SHOCK_2" in [p["type"] for p in options.get("promotions_available", [])],
        options.get("promotions_available"),
    )
    promoted = c.answers("promote_unit Shock II", "promote_unit", unit_id=KNIGHT_EAST, promotion="PROMOTION_SHOCK_2")
    c.equal("the knight reaches level 3", promoted["unit"]["level"], 3)
    hit = c.answers("the berserker attacks the camp again", "attack", unit_id=BERSERKER_EAST, x=51, y=35)
    c.equal("the pikeman weakens", (hit["target_hp"], hit["your_hp"]), (26, 33))
    c.answers("the swordsman reaches the camp", "move_unit", unit_id=SWORDSMAN, x=52, y=34)
    preview = c.answers("attack preview with no moves left", "attack", unit_id=SWORDSMAN, x=51, y=35, preview_only=True)
    c.check("the preview says the unit must wait", "no moves left" in preview.get("note", ""), preview)
    c.answers("the knight moves beside the camp", "move_unit", unit_id=KNIGHT_EAST, x=52, y=36)
    hit = c.answers("the knight attacks the camp", "attack", unit_id=KNIGHT_EAST, x=51, y=35)
    c.equal("the pikeman holds on", (hit["target_hp"], hit["your_hp"]), (5, 53))
    c.answers("set_production Granary in Kaupang", "set_production", city_id=KAUPANG, item="BUILDING_GRANARY")
    c.refuses("attack with no moves left", "attack", "no moves left", unit_id=SWORDSMAN, x=51, y=35)
    end_turn_and_wait(c, 159)


def turn_160(c):
    turn_begins(c, 160)
    hear_and_acknowledge(c, "the Who's Winning screen reaches the model", "Who's Winning")
    hit = c.answers("the swordsman takes the camp", "attack", unit_id=SWORDSMAN, x=51, y=35)
    c.equal("the camp's pikeman is destroyed", (hit.get("target_destroyed"), hit["your_hp"]), (True, 83))
    said, unread = wait_for_unread_message(c, "Captured a Worker (unit id %d)" % CAPTURED_WORKER)
    c.check(
        "the captured civilian's question reaches the model",
        said,
        "the unread game messages: %s" % ("; ".join(unread) or "none"),
    )
    c.check("the question is left for a human", "only a human can answer there" in (said or ""), said)
    c.check(
        "the World Congress's founding does not get past the open question",
        not any("World Congress" in message for message in unread),
        unread,
    )
    c.answers("acknowledge the captured civilian's question", "acknowledge_messages")
    c.equal("the camp's gold arrived", c.answers("overview after the camp", "overview")["gold"], 136)
    c.answers("set_production Bank in Aarhus", "set_production", city_id=AARHUS, item="BUILDING_BANK")
    c.answers("the berserker heals", "unit_action", unit_id=BERSERKER_EAST, action="heal")
    c.answers("the knight heals", "unit_action", unit_id=KNIGHT_EAST, action="heal")
    c.answers("the captured worker works on its own", "unit_action", unit_id=CAPTURED_WORKER, action="automate_work")
    demand = c.answers(
        "demand tribute from Songhai", "demand", civ="Songhai", get=[{"item": "gold_per_turn", "amount": 2}]
    )
    c.equal("Songhai gives in", demand["response"], "they gave in")
    hear_and_acknowledge(
        c, "Songhai's leader answers the demand", "Songhai's leader says: I will do what it takes to keep the peace."
    )
    congress = c.answers("congress after its founding", "congress")
    c.equal(
        "congress: the host, the proposals and when it meets",
        (congress["host"], [p["name"] for p in congress["proposals_awaiting_votes"]], congress["turns_until_session"]),
        ("Russia", ["ENACT: Embargo City-States", "ENACT: Ban Luxury: Citrus"], 16),
    )
    choices = [ch["text"] for p in congress["proposals_awaiting_votes"] for ch in p["vote_choices"]]
    c.check("congress names the vote choices without markup", choices and all("[" not in t for t in choices), choices)
    c.refuses(
        "vote_resolution before the session",
        "vote_resolution",
        "votes when it meets, in 16 turns",
        resolution_id=1,
        choice=0,
        votes=1,
    )
    _, reopened_by = end_turn_and_wait(c, 160)
    c.equal("the berserker's arrival reopened turn 160", reopened_by, ["Berserker %d at (32,16)" % BERSERKER_WEST])


def turn_161(c):
    briefing = turn_begins(c, 161)
    hear_and_acknowledge(c, "a withdrawn offer reaches the model", "Russia withdrew request #7 (deal_offer)")
    answer_requests(c, briefing, [("Poland", "deal_offer")], "accept")
    c.answers("set_research Astronomy", "set_research", tech="TECH_ASTRONOMY")
    spy = c.answers("spies in Zurich", "spies")["spies"][0]
    c.equal(
        "spies: Helle rigs Zurich's elections",
        (spy["city"], spy["state"], spy["coup_chance_percent"]),
        ("Zurich", "rigging elections", 13),
    )
    c.answers("promote_unit Drill II", "promote_unit", unit_id=SWORDSMAN, promotion="PROMOTION_DRILL_2")
    coup = c.answers("stage_coup in Zurich", "stage_coup", spy_index=0)
    c.equal("the coup succeeds", coup["succeeded"], True)
    c.answers("the swordsman fortifies", "unit_action", unit_id=SWORDSMAN, action="fortify")
    c.answers("the western berserker fortifies", "unit_action", unit_id=BERSERKER_WEST, action="fortify")
    c.answers("declare_war on Mogadishu", "declare_war", civ="Mogadishu")
    war = c.answers("briefing at war", "briefing")["diplomacy"]
    c.equal("one city-state is at war with Denmark", war.get("city_states_at_war_with_you"), 1)
    peace = c.answers("make_peace with Mogadishu", "city_state_action", civ="Mogadishu", action="make_peace")
    c.equal("Mogadishu is at peace again", peace["city_state"]["status"], "neutral")
    c.answers("move_spy: Helle to Gao", "move_spy", spy_index=0, civ="Songhai", city="Gao")
    c.refuses(
        "city_attack with no enemy in range", "city_attack", "no enemy is within its range", city_id=AARHUS, x=33, y=33
    )
    end_turn_and_wait(c, 161)


def turns_162_to_164(c):
    turn_begins(c, 162)
    hear_and_acknowledge(c, "another withdrawn offer reaches the model", "Russia withdrew request #9 (deal_offer)")
    c.answers("the missionary spreads Catholicism in Tyre", "unit_action", unit_id=MISSIONARY, action="spread_religion")
    end_turn_and_wait(c, 162)
    turn_begins(c, 163)
    cargo = unit_of_type(c, "UNIT_CARGO_SHIP", needs_orders=True)
    options = c.answers("trade_route_options for the returned cargo ship", "trade_route_options", unit_id=cargo["id"])
    c.check(
        "trade_route_options offers Zurich by sea",
        "Zurich" in [d["city"] for d in options["destinations"]],
        options["destinations"],
    )
    spent = c.answers("the missionary's last spread", "unit_action", unit_id=MISSIONARY, action="spread_religion")
    c.equal("unit_action says the missionary is gone", spent.get("unit_gone"), "the Missionary is gone")
    c.answers("set_production Walls in Kaupang", "set_production", city_id=KAUPANG, item="BUILDING_WALLS")
    route = c.answers(
        "establish_trade_route to Zurich",
        "establish_trade_route",
        unit_id=cargo["id"],
        x=34,
        y=38,
        type="international",
    )
    c.equal("the route goes to Zurich", route["route_to"], "Zurich")
    end_turn_and_wait(c, 163)
    briefing = turn_begins(c, 164)
    c.check(
        "the Golden Age's end is in the briefing",
        "Your Golden Age ends." in event_summaries(briefing),
        event_summaries(briefing),
    )
    opened = c.answers("adopt_policy opens Rationalism", "adopt_policy", policy="POLICY_BRANCH_RATIONALISM")
    c.equal("the branch opens", opened["result"], "branch opened")
    wealth = c.answers(
        "set_production Wealth in Copenhagen", "set_production", city_id=COPENHAGEN, item="PROCESS_WEALTH"
    )
    c.equal(
        "a process has no cost or turns",
        sorted(wealth["city"]["production"]),
        ["name", "ongoing"],
    )
    bought = c.answers("buy Walls in Kaupang", "buy", city_id=KAUPANG, item="BUILDING_WALLS")
    c.equal("the Walls' price in gold", bought["gold_spent"], 220)
    c.refuses(
        "end_turn while a bought building leaves Kaupang idle",
        "end_turn",
        "a city needs a production choice",
        summary="refused",
    )
    c.answers("set_production Market in Kaupang", "set_production", city_id=KAUPANG, item="BUILDING_MARKET")
    end_turn_and_wait(c, 164)


def turns_165_to_168(c):
    turn_begins(c, 165)
    relations = c.answers("discuss: denounce Songhai", "discuss", civ="Songhai", topic="denounce")["relations"]
    c.equal("the relations show Denmark's denouncement", relations.get("you_denounced_them"), True)
    met = c.answers("briefing after the denouncement", "briefing")["diplomacy"]["civilizations_met"]
    c.equal(
        "the briefing shows Denmark's denouncement",
        [m["civilization"] for m in met if m.get("you_denounced_them")],
        ["Songhai"],
    )
    hear_and_acknowledge(
        c, "Songhai's leader answers the denouncement", "Songhai's leader says: You have sullied your reputation"
    )
    c.refuses(
        "bully_gold without the military to back it",
        "city_state_action",
        "cannot be bullied for gold now",
        civ="Samarkand",
        action="bully_gold",
    )
    c.answers("the knight sleeps", "unit_action", unit_id=KNIGHT_EAST, action="sleep")
    end_turn_and_wait(c, 165)
    turn_begins(c, 166)
    hear_and_acknowledge(c, "Astronomy's pop-up reaches the model", "Researched Astronomy")
    c.answers("set_research Acoustics", "set_research", tech="TECH_ACOUSTICS")
    end_turn_and_wait(c, 166)
    turn_begins(c, 167)
    c.refuses(
        "set_production of a building the city's site rules out",
        "set_production",
        "needs a mountain next to the city",
        city_id=AARHUS,
        item="BUILDING_OBSERVATORY",
    )
    c.answers("the eastern berserker fortifies", "unit_action", unit_id=BERSERKER_EAST, action="fortify")
    c.answers("set_production Musketman in Aarhus", "set_production", city_id=AARHUS, item="UNIT_MUSKETMAN")
    end_turn_and_wait(c, 167)
    briefing = turn_begins(c, 168)
    request = briefing["diplomacy_requests"][0]
    c.check("leaders' requests carry no markup", "[" not in request["message"], request["message"])
    answer_requests(c, briefing, [("Songhai", "statement")], "acknowledge")


def turn_168_pop_ups_raised_through_lua(c):
    game_lua(GREAT_WORK_LOOKED_AT)
    said, _ = wait_for_unread_message(c, "great work", seconds=5)
    c.equal("a Great Work the player clicks to look at sends no game message", said, None)
    game_lua(GREAT_WORK_RECEIVED)
    wait_for_unread_message(c, "great work")
    # A leaked second message would come frames after the first; counting at once would miss it.
    time.sleep(3)
    _, unread = wait_for_unread_message(c, "great work", seconds=0)
    c.equal(
        "only the Great Work received from another civilization reaches the model",
        [m for m in unread if "great work" in m],
        ["Received the great work A True Story by Lucian of Samosata (Great Writer), now in Delhi"],
    )
    c.answers("acknowledge the Great Work", "acknowledge_messages")
    game_lua(CONGRESS_FOUNDED)
    hear_and_acknowledge(
        c,
        "the World Congress's founding reaches the model",
        "The World Congress was founded, hosted by Russia (congress shows when it meets and what is proposed)",
    )
    game_lua(CONGRESS_SPECIAL_SESSION)
    hear_and_acknowledge(
        c,
        "a special session of the World Congress reaches the model",
        "The World Congress has called a special session, hosted by Russia (congress shows what it votes on)",
    )


def turn_168_war_on_russia(c):
    declared = c.answers("declare_war on Russia", "declare_war", civ="Russia")
    c.equal("Denmark is at war with Russia", declared["at_war_with"], "Russia")
    met = c.answers("briefing at war with Russia", "briefing")["diplomacy"]["civilizations_met"]
    c.equal(
        "Russia's teammate Poland is at war with Denmark too",
        sorted(m["civilization"] for m in met if m["at_war"]),
        ["Poland", "Russia"],
    )
    c.answers("the eastern berserker moves next to Rostov", "move_unit", unit_id=BERSERKER_EAST, x=54, y=36)
    hit = c.answers("the berserker attacks Rostov", "attack", unit_id=BERSERKER_EAST, **ROSTOV_TILE)
    c.equal("Rostov's and the berserker's hit points after the attack", (hit["target_hp"], hit["your_hp"]), (172, 71))
    c.answers("the knight moves next to Rostov", "move_unit", unit_id=KNIGHT_EAST, x=53, y=37)
    c.answers("the knight sleeps", "unit_action", unit_id=KNIGHT_EAST, action="sleep")


def turns_168_to_170_with_a_second_model(c):
    india = second_model(c)
    taken = india.answers("the second model takes India's seat", "take_seat", civ="India")
    india.equal("the second model plays India", taken["playing"], "India")
    offer = c.answers(
        "propose_deal to India's seat",
        "propose_deal",
        civ="India",
        give=[{"item": "resource", "resource": "Gold", "amount": 1}],
        get=[{"item": "resource", "resource": "Incense", "amount": 1}],
    )
    c.equal("the offer goes to the model in India's seat", offer.get("proposed_to_human"), "India")
    requests = india.answers("diplomacy for India", "diplomacy")["requests"]
    india.equal(
        "India's seat holds Denmark's offer",
        [(r["from"], r["kind"]) for r in requests],
        [("Denmark", "human_deal_offer")],
    )
    india.refuses("answer_deal outside India's turn", "answer_deal", "not your turn", civ="Denmark", accept=True)
    ended = c.answers("end_turn 168", "end_turn", summary="mcp_checks: war on Russia.")
    c.equal("end_turn ends turn 168", ended["ended_turn"], 168)
    status = wait_for_turn(india, 168)
    india.check("India's turn says Denmark's offer waits", "deal offer" in status.get("diplomacy_waiting", ""), status)
    answered = india.answers("answer_deal: India accepts", "answer_deal", civ="Denmark", accept=True)
    india.equal("the deal is accepted", answered["answered"], "accepted")
    india.answers("testing_mode on for India", "testing_mode", enabled=True)
    advanced = india.answers(
        "advance_round from India's seat", "advance_round", summary="mcp_checks: India passes.", force=True
    )
    india.equal(
        "advance_round plays on to turn 169 and leaves Denmark's turn to the model playing it",
        (advanced["round_advanced"], advanced["turn"], advanced["is_my_turn"]),
        (True, 169, False),
    )
    india.answers("testing_mode off for India", "testing_mode", enabled=False)
    status = wait_for_turn(c, 169)
    c.equal("Denmark's turn 169 comes", (status["civilization"], status["turn"]), ("Denmark", 169))
    hear_and_acknowledge(c, "India's answer reaches Denmark", "India accepted your offer; the deal is in force")
    c.refuses(
        "make_peace with Russia a turn into the war",
        "make_peace",
        "not willing to make peace yet",
        civ="Russia",
        preview_only=True,
    )
    c.answers("the berserker fortifies", "unit_action", unit_id=BERSERKER_EAST, action="fortify")
    ended = c.answers("end_turn 169", "end_turn", summary="mcp_checks: turn 169.")
    c.equal("end_turn ends turn 169", ended["ended_turn"], 169)
    status = wait_for_turn(india, 169)
    india.equal("India's turn 169 comes", (status["civilization"], status["turn"]), ("India", 169))
    hear_and_acknowledge(india, "India's great person reaches its model", "A Great Artist has appeared")
    india.refuses("release_seat during India's own turn", "release_seat", "end your turn first")
    pending = india.answers("briefing for India on turn 169", "briefing")["decisions_pending"]
    india.equal(
        "India's cities with nothing to build",
        [p for p in pending if p.startswith("production:")],
        [
            "production: Delhi (id %d) is building nothing (city_options, set_production)" % DELHI,
            "production: Vijayanagara (id %d) is building nothing (city_options, set_production)" % VIJAYANAGARA,
        ],
    )
    for city in (DELHI, VIJAYANAGARA):
        india.answers(
            "set_production Wealth in India's city %d" % city, "set_production", city_id=city, item="PROCESS_WEALTH"
        )
    india.answers("end_turn for India", "end_turn", summary="mcp_checks: India passes.", force=True)
    hear_and_acknowledge(india, "a missionary's blocked path reopens India's turn", "will not continue to")
    india.answers("end_turn for India again", "end_turn", summary="mcp_checks: India passes.", force=True)
    status = wait_for_turn(c, 170)
    c.equal("Denmark's turn 170 comes", (status["civilization"], status["turn"]), ("Denmark", 170))
    india.answers("release_seat after India's turn", "release_seat")


def turn_170(c):
    turn_begins(c, 170)
    before = c.answers("unit_options for the wounded berserker", "unit_options", unit_id=BERSERKER_EAST)
    c.check(
        "the berserker can pillage the Russian tile it stands on", "pillage" in before["actions"], before["actions"]
    )
    pillaged = c.answers("the berserker pillages", "unit_action", unit_id=BERSERKER_EAST, action="pillage")
    c.equal("pillaging heals the berserker", (before["unit"]["hp"], pillaged["unit"]["hp"]), (29, 54))
    c.answers("the berserker falls back", "move_unit", unit_id=BERSERKER_EAST, x=52, y=34)
    c.refuses(
        "move_unit onto a teammate's unit",
        "move_unit",
        "teammate India's Spearman stands on that tile",
        unit_id=KNIGHT_EAST,
        x=51,
        y=34,
    )
    revoked = c.answers(
        "revoke_protection of Samarkand", "city_state_action", civ="Samarkand", action="revoke_protection"
    )
    c.equal("Samarkand is no longer under Denmark's protection", revoked["city_state"]["protected_by_you"], False)
    c.answers(
        "discuss: ask Spain to join the war on Russia", "discuss", civ="Spain", topic="joint_war", target="Russia"
    )
    said, unread = wait_for_unread_message(c, "Spain's leader says")
    c.check("Spain's answer reaches the model", said, unread)
    c.refuses(
        "an order while a game message is unread", "move_unit", "unread game message", unit_id=KNIGHT_EAST, x=52, y=33
    )
    c.answers("acknowledge Spain's answer", "acknowledge_messages")
    moved = c.answers("the knight heads north", "move_unit", unit_id=KNIGHT_EAST, x=52, y=33)
    c.equal("a two-turn move that has begun", (moved["estimated_turns_total"], moved["now_at"]), (2, "(53,34)"))
    sent = c.answers(
        "move_spy: Helle to Madrid as a diplomat", "move_spy", spy_index=0, civ="Spain", city="Madrid", as_diplomat=True
    )
    c.equal("Helle travels to Madrid", sent["travelling_to"], "Madrid")
    university = dict(city_id=COPENHAGEN, building="BUILDING_UNIVERSITY", remove=True)
    c.answers("city_specialist: a scientist leaves Copenhagen's University", "city_specialist", **university)
    c.refuses("city_specialist: none left to remove", "city_specialist", "no specialist works there", **university)
    end_turn_and_wait(c, 170)


def turn_171(c):
    turn_begins(c, 171)
    hear_and_acknowledge(c, "Acoustics' pop-up reaches the model", "Researched Acoustics")
    c.answers("set_research Metallurgy", "set_research", tech="TECH_METALLURGY")
    c.answers("set_production Harbor in Kaupang", "set_production", city_id=KAUPANG, item="BUILDING_HARBOR")
    c.answers("the berserker returns next to Rostov", "move_unit", unit_id=BERSERKER_EAST, x=54, y=36)
    preview = c.answers(
        "move preview for a unit with no moves left", "move_unit", unit_id=BERSERKER_EAST, x=53, y=35, preview_only=True
    )
    c.equal(
        "the preview says the order must wait",
        preview.get("note"),
        "this unit has no moves left this turn; give the order next turn",
    )
    c.refuses(
        "move_unit with no moves left", "move_unit", "no moves left this turn", unit_id=BERSERKER_EAST, x=53, y=35
    )
    c.refuses(
        "a melee attack from two tiles away",
        "attack",
        "need the unit adjacent",
        unit_id=KNIGHT_EAST,
        preview_only=True,
        **ROSTOV_TILE,
    )
    c.answers("the knight returns next to Rostov", "move_unit", unit_id=KNIGHT_EAST, x=53, y=37)
    game_lua(PLACE_RUSSIAN_MUSKETMAN)
    threats = c.answers("briefing with a Russian musketman by Copenhagen", "briefing")["threats"]
    c.check(
        "the briefing names the musketman as a threat",
        any(t["unit"] == "Musketman" and t["at"] == "(%(x)d,%(y)d)" % MUSKETMAN_TILE for t in threats),
        threats,
    )
    bombard = c.answers(
        "city_attack: Copenhagen bombards the musketman", "city_attack", city_id=COPENHAGEN, **MUSKETMAN_TILE
    )
    c.equal("the bombard hurts the musketman", (bombard["target_hp_before"], bombard["target_hp"]), (100, 68))
    c.refuses(
        "a trebuchet's attack before it sets up",
        "attack",
        "must set up before it can fire",
        unit_id=TREBUCHET_FARM,
        preview_only=True,
        **MUSKETMAN_TILE,
    )
    set_up = c.answers("the trebuchet sets up", "unit_action", unit_id=TREBUCHET_FARM, action="set_up")
    c.equal("the trebuchet is set up to fire", set_up["unit"]["set_up_to_fire"], True)
    fired = c.answers("the trebuchet fires", "attack", unit_id=TREBUCHET_FARM, **MUSKETMAN_TILE)
    c.equal(
        "a ranged attack hurts the musketman and costs the trebuchet nothing",
        (fired["estimate"]["mode"], fired["target_hp"], fired["damage_taken"]),
        ("ranged", 47, 0),
    )
    c.refuses(
        "a second bombard in one turn",
        "city_attack",
        "already attacked this turn",
        city_id=COPENHAGEN,
        **MUSKETMAN_TILE,
    )
    end_turn_and_wait(c, 171)


def turn_172_rostov_captured(c):
    briefing = turn_begins(c, 172)
    answer_requests(c, briefing, [("Spain", "statement")], "acknowledge")
    c.equal(
        "turn 160's question about the captured worker is still open", game_lua(QUESTION_OPEN, GENERIC_POPUP), ["true"]
    )
    game_lua(KEEP_CAPTURED_WORKER, GENERIC_POPUP)
    c.equal("the human's answer closes the question", game_lua(QUESTION_OPEN, GENERIC_POPUP), ["false"])
    said, _ = wait_for_unread_message(c, "World Congress", seconds=5)
    c.equal("the World Congress's founding, held back by the question, never arrives", said, None)
    game_lua(ROSTOV_AT_ONE_HP)
    decide = "captured city: decide the fate of Rostov (id %d) with city_task: annex, puppet or raze" % ROSTOV
    hit = c.answers("the knight attacks Rostov at 1 hit point", "attack", unit_id=KNIGHT_EAST, **ROSTOV_TILE)
    c.equal(
        "the knight takes Rostov, and the attack offers what the game offers, without liberating it to Russia",
        (hit.get("city_captured"), hit.get("note")),
        (True, decide),
    )
    briefing = c.answers("briefing after the capture", "briefing")
    c.check(
        "the briefing asks what becomes of Rostov, and not what it builds",
        decide in briefing["decisions_pending"]
        and not any(p.startswith("production: Rostov") for p in briefing["decisions_pending"]),
        briefing["decisions_pending"],
    )
    c.equal(
        "status, overview and the briefing name Rostov's fate as what blocks the turn",
        [
            c.answers("status", "status")["end_turn_blocking"],
            c.answers("overview", "overview")["end_turn_blocking"],
            briefing["end_turn_blocking"],
        ],
        [decide] * 3,
    )
    c.refuses(
        "end_turn while Rostov's fate is open",
        "end_turn",
        "captured city: decide the fate of Rostov",
        summary="refused",
    )
    c.refuses(
        "liberate Rostov to the civilization it was taken from",
        "city_task",
        "cannot be liberated",
        city_id=ROSTOV,
        task="liberate",
    )
    puppet = c.answers("city_task: Rostov becomes a puppet", "city_task", city_id=ROSTOV, task="puppet")
    c.equal("Rostov is a puppet", puppet["city"].get("puppet"), True)
    food = puppet["city"]
    c.check(
        "Rostov starves by less than one food a turn, and its food surplus shows it",
        -1 < food["food_surplus_per_turn"] < 0 and "starving" in food.get("not_growing", ""),
        (food["food_surplus_per_turn"], food.get("not_growing")),
    )
    after = c.answers("briefing after the decision", "briefing")
    c.check(
        "the briefing no longer asks about Rostov", decide not in after["decisions_pending"], after["decisions_pending"]
    )
    c.check(
        "the briefing warns that Rostov starves, though by less than one food",
        any(w.startswith("starvation: Rostov loses 0.") for w in after["warnings"]),
        after["warnings"],
    )
    c.refuses("puppet a puppet", "city_task", "already a puppet", city_id=ROSTOV, task="puppet")
    annexed = c.answers("city_task: annex the puppet", "city_task", city_id=ROSTOV, task="annex")
    c.equal("Rostov is annexed", annexed["city"].get("puppet"), None)
    razing = c.answers("city_task: raze Rostov", "city_task", city_id=ROSTOV, task="raze")
    c.equal("Rostov burns a turn for each of its people", razing["city"].get("razing_turns_left"), 2)
    c.refuses(
        "city_attack from a city in resistance", "city_attack", "resistance or being razed", city_id=ROSTOV, x=55, y=38
    )
    spared = c.answers("city_task: stop razing Rostov", "city_task", city_id=ROSTOV, task="stop_razing")
    c.equal("Rostov no longer burns", spared["city"].get("razing_turns_left"), None)
    c.refuses(
        "stop_razing a city that is not burning", "city_task", "not being razed", city_id=ROSTOV, task="stop_razing"
    )


def turn_172_trebuchet_and_spy(c):
    c.refuses(
        "set_up for a trebuchet already set up",
        "unit_action",
        "already set up stays set up",
        unit_id=TREBUCHET_FARM,
        action="set_up",
    )
    spy = c.answers("spies with Helle in Madrid", "spies")["spies"][0]
    c.equal(
        "Helle is Denmark's diplomat in Madrid",
        (spy["city"], spy.get("diplomat"), spy["state"]),
        ("Madrid", True, "making introductions"),
    )


def refusals_at_turn_172(c):
    """Tools the game at turn 172 gives nothing to act on refuse and say why, and two of them describe what is missing."""
    c.refuses(
        "propose_resolution with no proposal of Denmark's due",
        "propose_resolution",
        "cannot propose now",
        resolution="RESOLUTION_EMBARGO",
    )
    c.refuses("abstain before the session", "abstain", "votes when it meets")
    ideologies = c.answers("choose_ideology lists the ideologies", "choose_ideology")
    c.equal(
        "choose_ideology: none can be chosen yet",
        (ideologies["can_choose_now"], len(ideologies["ideologies"])),
        (False, 3),
    )
    c.refuses("switch_ideology without one", "switch_ideology", "no ideology yet")
    c.refuses("launch_spaceship without a spaceship", "launch_spaceship", "spaceship is not ready")
    c.refuses(
        "add_reformation_belief without the policy",
        "add_reformation_belief",
        "no policy of yours grants",
        belief="BELIEF_JESUIT_EDUCATION",
    )
    c.refuses(
        "found_pantheon after founding a religion",
        "found_pantheon",
        "you already have a pantheon",
        belief="BELIEF_NONEXISTENT",
    )
    c.refuses(
        "found_religion with no prophet's start",
        "found_religion",
        "no religion founding is waiting",
        religion="RELIGION_ISLAM",
        beliefs=["BELIEF_TITHE", "BELIEF_PAGODAS"],
    )
    c.refuses(
        "choose_great_person with none owed",
        "choose_great_person",
        "no free great person",
        unit="UNIT_SCIENTIST",
        source="free",
    )
    c.refuses(
        "steal_tech with nothing stolen",
        "steal_tech",
        "no stolen technology is waiting",
        civ="Songhai",
        tech="TECH_ASTRONOMY",
    )
    c.refuses(
        "city_task annex on Denmark's capital", "city_task", "already yours outright", city_id=COPENHAGEN, task="annex"
    )
    c.refuses(
        "answer_deal for a computer player's offer",
        "answer_deal",
        "only a human proposes deals",
        civ="India",
        accept=True,
    )
    c.refuses("advance_round outside testing mode", "advance_round", "testing tool", summary="refused")
    c.refuses("upgrade_unit for a worker", "upgrade_unit", "no upgrade exists", unit_id=WORKER)
    c.refuses("archaeology with no dig", "archaeology", "no completed dig", choice="landmark")
    c.refuses(
        "move_great_work between different kinds of slot",
        "move_great_work",
        "hold different kinds of great work",
        from_city_id=COPENHAGEN,
        from_building="BUILDING_PALACE",
        from_slot=0,
        to_city_id=AARHUS,
        to_building="BUILDING_AMPHITHEATER",
        to_slot=0,
    )
    pantheon = c.answers("religion_options after the religion's founding", "religion_options")["pantheon"]
    c.equal("religion_options: the pantheon is part of the religion", pantheon.get("have"), True)


def turn_172_moscow_taken_from_poland(c):
    moscow = int(game_lua(MOSCOW_TAKEN_FROM_POLAND)[0])
    said, unread = wait_for_unread_message(c, "Captured Moscow (city id %d)" % moscow)
    c.check(
        "the game's question about Moscow reaches the model, with liberation back to its founder",
        (said or "").endswith("Decide its fate with city_task: annex, puppet or liberate (Russia)"),
        said or unread,
    )
    c.answers("acknowledge the capture of Moscow", "acknowledge_messages")
    freed = c.answers("city_task: liberate Moscow", "city_task", city_id=moscow, task="liberate")
    c.equal("Moscow goes back to Russia", freed["liberated_to"], "Russia")
    c.equal("the game's question about Moscow waits on screen", game_lua(QUESTION_OPEN, GENERIC_POPUP), ["true"])
    game_lua(CLOSE_QUESTION, GENERIC_POPUP)
    moscow = int(game_lua(MOSCOW_TAKEN_FROM_POLAND)[0])
    hear_and_acknowledge(c, "Moscow's second capture reaches the model", "Captured Moscow (city id %d)" % moscow)
    annexed = c.answers("city_task: annex Moscow at once", "city_task", city_id=moscow, task="annex")
    c.equal("Moscow is annexed, not a puppet", annexed["city"].get("puppet"), None)
    c.refuses(
        "liberate a city whose fate is already decided",
        "city_task",
        "cannot be liberated",
        city_id=moscow,
        task="liberate",
    )
    c.equal(
        "the game's question about the second capture waits on screen", game_lua(QUESTION_OPEN, GENERIC_POPUP), ["true"]
    )
    game_lua(CLOSE_QUESTION, GENERIC_POPUP)


def turn_172_free_choices(c):
    game_lua(FREE_CHOICES)
    pending = c.answers("briefing with a free technology", "briefing")["decisions_pending"]
    c.check(
        "the briefing names the free technology",
        any(p.startswith("free_tech: 1 free technology to claim") for p in pending),
        pending,
    )
    claimed = c.answers("set_research claims the free technology", "set_research", tech="TECH_METALLURGY")
    c.equal("Metallurgy comes free", claimed.get("free_tech_claimed"), "Metallurgy")
    hear_and_acknowledge(c, "Metallurgy's pop-up reaches the model", "Researched Metallurgy")
    person = c.answers("choose_great_person: the free one", "choose_great_person", unit="UNIT_SCIENTIST", source="free")
    c.equal("a Great Scientist appears", person["unit"], "Great Scientist")
    hear_and_acknowledge(c, "the Great Scientist's pop-up reaches the model", "A Great Scientist has appeared")
    policies = c.answers("policy_options with a free policy", "policy_options")
    c.equal("one free policy can be adopted", (policies["free_policies"], policies["can_adopt_now"]), (1, True))
    rationalism = next(b for b in policies["branches"] if b["type"] == "POLICY_BRANCH_RATIONALISM")
    policy = next(p["type"] for p in rationalism["policies"] if p.get("can_adopt_now"))
    adopted = c.answers("adopt_policy with the free policy", "adopt_policy", policy=policy)
    c.equal(
        "the free policy costs no culture",
        (adopted["result"], adopted["culture"]),
        ("policy adopted", policies["culture"]),
    )
    reformation = c.answers("religion_options with a reformation belief due", "religion_options")["reformation"]
    belief = reformation["reformation_beliefs"][0]["type"]
    added = c.answers("add_reformation_belief", "add_reformation_belief", belief=belief)
    c.check(
        "the reformation belief joins Denmark's religion",
        belief in [b["type"] for b in added["religion"]["beliefs"]],
        added["religion"]["beliefs"],
    )
    c.refuses("a second reformation belief", "add_reformation_belief", "already added", belief=belief)


SCENARIO = [
    connecting,
    naming,
    turn_150_status,
    spies_religion_and_neighbours,
    works_and_the_empire,
    research_and_policies,
    every_unit_city_and_civ,
    deal_preview,
    refusals,
    turn_150_actions,
    turn_151,
    seats,
    turn_152,
    turn_153,
    turn_154,
    turn_155,
    turn_156,
    turn_157,
    turn_158,
    turn_159,
    turn_160,
    turn_161,
    turns_162_to_164,
    turns_165_to_168,
    turn_168_pop_ups_raised_through_lua,
    turn_168_war_on_russia,
    turns_168_to_170_with_a_second_model,
    turn_170,
    turn_171,
    turn_172_rostov_captured,
    turn_172_trebuchet_and_spy,
    refusals_at_turn_172,
    turn_172_moscow_taken_from_poland,
    turn_172_free_choices,
]
# Steps that only look: when one fails, the steps after it still find the game as they expect.
READ_ONLY = {
    turn_150_status,
    spies_religion_and_neighbours,
    works_and_the_empire,
    research_and_policies,
    every_unit_city_and_civ,
    deal_preview,
}


def run():
    """Plays the scenario in order and returns its results. A step that changes the game, or may have, and ends early
    stops the scenario, since every later step expects the game as the earlier ones left it."""
    c = Checks(Mcp())
    for number, step in enumerate(SCENARIO):
        print(step.__name__.replace("_", " "), flush=True)
        try:
            step(c)
        except StepStopped:
            pass
        except Exception as e:  # a missing field or a dropped connection
            c.check("%s ran to the end" % step.__name__, False, "%s: %s" % (type(e).__name__, e))
        else:
            continue
        if step not in READ_ONLY:
            skipped = [s.__name__ for s in SCENARIO[number + 1 :]]
            if skipped:
                c.check("the scenario ran to the end", False, "not run: " + ", ".join(skipped))
            break
    return c.results


def main():
    results = run()
    failed = [r for r in results if not r.passed]
    print("%d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

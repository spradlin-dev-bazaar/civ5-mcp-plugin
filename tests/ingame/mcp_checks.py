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

# The save's own ids: Denmark's cities and units at turn 150.
COPENHAGEN, AARHUS, KAUPANG = 8192, 16385, 24578
GREAT_WRITER, WORKER, CARAVAN = 344074, 90119, 286732
IDLE_UNITS = {
    237568: ("Berserker", "fortify"),
    327684: ("Trebuchet", "sleep"),
    204806: ("Knight", "sleep"),
    335885: ("Knight", "sleep"),
}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

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
        {"Copenhagen": ["TXT_KEY_GREAT_HENRY_VIII", "TXT_KEY_GREAT_WORK_CHU_CI"], "Aarhus": ["empty"]},
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
        ["TXT_KEY_GREAT_HENRY_VIII", "TXT_KEY_GREAT_WORK_CHU_CI"],
    )
    c.check("the new work fills Aarhus's empty slot", titles.get("Aarhus") not in (None, ["empty"]), titles)
    c.equal("the treasury before buying a tile", c.answers("overview", "overview")["gold"], 67)
    c.answers("buy_tile (39,23) for Copenhagen", "buy_tile", city_id=COPENHAGEN, x=39, y=23)
    c.equal("buy_tile takes the tile's price from the treasury", c.answers("overview", "overview")["gold"], 37)
    c.answers("the worker builds a road", "unit_action", unit_id=WORKER, action="build", build="BUILD_ROAD")
    for unit, (name, action) in IDLE_UNITS.items():
        c.answers("%s %d: %s" % (name, unit, action), "unit_action", unit_id=unit, action=action)
    c.answers("log a line", "log", text="mcp_checks: turn 150 orders given")
    status, reopened_by = end_turn_and_wait(c, 150, "mcp_checks: Harbor, a great work, a tile and a road.")
    c.equal("Denmark's turn 151 comes", (status["civilization"], status["turn"]), ("Denmark", 151))
    c.equal(
        "units that finished their moves with moves left reopened turn 150",
        reopened_by,
        ["Berserker 262155 at (47,28)"],
    )


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


def end_turn_and_wait(c, turn, summary, tries=5):
    """Ends the turn and waits for the next, as a model does: when the turn comes back early, because a unit finished a
    queued move with moves to spare, the idle units skip and the turn ends again. Returns the next turn's status and
    the units that brought the turn back, in order."""
    reopened_by = []
    for _ in range(tries):
        ended = c.mcp.call("end_turn", summary=summary)
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


def answer_requests(c, briefing, expected):
    """Answers every request from another leader with its first choice, as the checks expect them to arrive."""
    requests = briefing["diplomacy_requests"]
    c.equal("the requests from other leaders", [(r["from"], r["kind"]) for r in requests], expected)
    for r in requests:
        choice = r["choices"].split(", ")[0]
        c.answers(
            "respond_to_request: %s to %s's %s" % (choice, r["from"], r["kind"]),
            "respond_to_request",
            id=r["id"],
            choice=choice,
        )


def turn_151(c):
    briefing = c.answers("briefing on turn 151", "briefing")
    c.equal("the briefing is for turn 151", briefing["turn"], 151)
    c.answers("acknowledge_messages", "acknowledge_messages")
    answer_requests(c, briefing, [("Songhai", "deal_offer")])
    reply = c.answers("discuss: ask Russia for friendship", "discuss", civ="Russia", topic="ask_friendship")
    c.check("discuss gives Russia's reply", reply.get("their_reply"), reply)
    said, unread = wait_for_message(c, "Russia's leader says")
    c.check(
        "Russia's leader screen reaches the model as a game message",
        said,
        "no unread game message with McpPopups.cpp's wording; the unread ones: %s" % ("; ".join(unread) or "none"),
    )
    c.answers("acknowledge the leader's words", "acknowledge_messages")


def wait_for_message(c, text, seconds=20):
    """The unread game message containing text, or None, and the unread messages: the game reports it a few frames
    after the event."""
    deadline = time.time() + seconds
    while True:
        unread = [m["text"] for m in c.mcp.call("briefing")["game_messages"] if m.get("unread")]
        said = next((message for message in unread if text in message), None)
        if said or time.time() >= deadline:
            return said, unread
        time.sleep(1)


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

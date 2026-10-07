// The bridge between zchat and the Pokémon Showdown battle simulator, for /game pokemon (see src/pokemon.cpp).
// zchat writes this file next to its own copy of the simulator (installed with npm in its config folder) and runs it
// with node, for each battle it referees:
//
//   node bridge.js FORMAT P1-NAME P2-NAME
//
// where FORMAT is one of Showdown's random battles, gen1randombattle to gen9randombattle.
//
// It reads commands on stdin, one per line:
//   p1 CHOICE | p2 CHOICE    a player's choice, as Showdown takes it ("move 1", "switch 3", "move 2 terastallize",
//                            "move 1 mega", "move 3 zmove", "move 2 dynamax")
//   forcewin p1|p2           ends the battle with that player winning (the timer ran out, a player left)
//   forcetie                 ends it with no winner
// and prints on stdout, one per line, what the simulator says, as "STREAM TEXT", where STREAM is spectator (what
// everyone may see), p1 or p2 (what only that player may see: their requests, their side's exact HP...), and TEXT is
// the message with backslashes doubled and line breaks as \n. "ready" is printed once it runs; "exit" when it ends.
//
// What zchat cannot know without the Pokédex (the battle's generation's) is added to the messages: each move of a
// request gets its type, category, base power, accuracy and short description, and so do its Z-Moves and Max Moves;
// each Pokémon of a request gets its types, and the request its rqid; and after every line showing a Pokémon
// (switching in, changing forme) comes "|zchat-types|POKEMON|TYPE1/TYPE2", and before every move used,
// "|zchat-move|TYPE|CATEGORY|TARGET|contact" (TARGET as the Pokédex has it: "normal", "self", "foeSide"...; "contact"
// only for one that makes contact), for the battle window to play it.

"use strict";

const {BattleStream, getPlayerStreams, Dex: AllDex} = require("pokemon-showdown");

const [given, p1, p2] = process.argv.slice(2);
const format = /^gen[1-9]randombattle$/.test(given || "") ? given : "gen9randombattle";
// Types and moves as they were in that generation (Clefairy was Normal, Bite was Normal).
const Dex = AllDex.forFormat(format);
const stream = new BattleStream();
const streams = getPlayerStreams(stream);

const escape = text => text.replace(/\\/g, "\\\\").replace(/\r?\n/g, "\\n");
const out = (tag, text) => process.stdout.write(`${tag} ${escape(text)}\n`);

// "Volbeat, L90, M" is a Volbeat.
const typesOf = details => Dex.species.get(details.split(",")[0]).types.join("/");

// The stream's requests have no rqid (Showdown's server adds it): each player's are numbered here, for zchat to tell
// which request a choice answers.
const rqids = {p1: 0, p2: 0};

// A move of a request (by id or name), with what the Pokédex says of it.
function describe(m, id) {
    const move = Dex.moves.get(id);
    if (!move.exists) return;
    Object.assign(m, {type: move.type, category: move.category, basePower: move.basePower, accuracy: move.accuracy,
                      desc: move.shortDesc || move.desc});
}

function enrichRequest(json, tag) {
    const request = JSON.parse(json);
    if (!request.rqid && rqids[tag] !== undefined) request.rqid = ++rqids[tag];
    for (const active of request.active || []) {
        for (const m of active.moves || []) describe(m, m.id);
        // Z-Moves and Max Moves, one per move (a Z-Move may be null): their power is the move's they come from (the
        // Pokédex has a placeholder for theirs), except for status ones.
        const based = (list, power) => (list || []).forEach((g, i) => {
            if (!g) return;
            describe(g, g.move);
            const base = Dex.moves.get((active.moves[i] || {}).id);
            // A status move's Z-Move ("Z-Happy Hour") is the move itself, with a Z-Power effect on top.
            if (!g.type && base.exists) {
                Object.assign(g, {type: base.type, category: base.category, basePower: 0, accuracy: base.accuracy,
                                  desc: `${base.shortDesc || base.desc} Z-Power: an extra effect first.`});
            }
            if (base.exists && g.category !== "Status" && power(base)) g.basePower = power(base);
        });
        based(active.canZMove, base => base.zMove && base.zMove.basePower);
        based(active.maxMoves && active.maxMoves.maxMoves, base => base.maxMove && base.maxMove.basePower);
    }
    for (const p of (request.side && request.side.pokemon) || []) {
        p.types = typesOf(p.details);
        const item = Dex.items.get(p.item), ability = Dex.abilities.get(p.ability);
        if (item.exists) p.itemName = item.name;
        if (ability.exists) p.abilityName = ability.name;
        p.moveNames = (p.moves || []).map(id => Dex.moves.get(id).name || id);
    }
    return JSON.stringify(request);
}

function enrich(chunk, tag) {
    const lines = [];
    for (const line of chunk.split("\n")) {
        if (line.startsWith("|request|") && line.length > 9) {
            try {
                lines.push("|request|" + enrichRequest(line.slice(9), tag));
                continue;
            } catch {}
        }
        const parts = line.split("|");
        if (parts[1] === "move" && parts[3]) {
            const move = Dex.moves.get(parts[3]);
            if (move.exists) {
                lines.push(`|zchat-move|${move.type}|${move.category}|${move.target}|${move.flags.contact ? "contact" : ""}`);
            }
        }
        lines.push(line);
        if (["switch", "drag", "replace", "detailschange", "-formechange"].includes(parts[1]) && parts[3]) {
            lines.push(`|zchat-types|${parts[2]}|${typesOf(parts[3])}`);
        }
    }
    return lines.join("\n");
}

for (const tag of ["spectator", "p1", "p2"]) {
    (async () => {
        for await (const chunk of streams[tag]) out(tag, enrich(chunk, tag));
    })().catch(e => out("error", String(e && e.stack || e)));
}
(async () => {
    // The omniscient stream ends when the battle does.
    for await (const chunk of streams.omniscient) void chunk;
    out("exit", "");
    setTimeout(() => process.exit(0), 200);
})();

streams.omniscient.write(`>start ${JSON.stringify({formatid: format})}
>player p1 ${JSON.stringify({name: p1 || "Player 1"})}
>player p2 ${JSON.stringify({name: p2 || "Player 2"})}`);
out("ready", "");

let pending = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", data => {
    pending += data;
    let nl;
    while ((nl = pending.indexOf("\n")) >= 0) {
        const line = pending.slice(0, nl).trim();
        pending = pending.slice(nl + 1);
        const space = line.indexOf(" ");
        const command = space < 0 ? line : line.slice(0, space);
        const rest = space < 0 ? "" : line.slice(space + 1);
        if ((command === "p1" || command === "p2") && rest) {
            streams.omniscient.write(`>${command} ${rest}`);
        } else if (command === "forcewin" && (rest === "p1" || rest === "p2")) {
            streams.omniscient.write(`>forcewin ${rest}`);
        } else if (command === "forcetie") {
            streams.omniscient.write(">forcetie");
        }
    }
});
// zchat went away: so does the battle.
process.stdin.on("end", () => process.exit(0));

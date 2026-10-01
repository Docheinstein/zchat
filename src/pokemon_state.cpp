// What a Pokémon Showdown battle looks like to one of its viewers, from the battle's messages (see pokemon_state.hpp).
//
// The state is what Showdown's client keeps: both teams as far as they are known (ours from the requests, which tell
// everything about it; the opponent's from what happens), the field, the turn and the request waiting for us. The log
// is what the client's battle log says, in its words: the texts come from Showdown's own table (data/text in
// pokemon-showdown, the same one the client uses), put together as its BattleTextParser does, so "Volbeat used
// Thunder Wave!", "The opposing Annihilape is paralyzed! It may be unable to move!" and "Pointed stones float in the
// air around the opposing team!" read exactly as on Showdown.
//
// The messages come from the network (through the battle's referee): anything malformed is ignored, never trusted.

#include "pokemon_state.hpp"

#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <initializer_list>
#include <utility>

namespace zchat::pokemon {

namespace {

    // Showdown's battle texts, as {id, key, text}: the id is that of a move, ability, item, status, weather... or
    // "default"; the key what happened ("start", "end", "damage", "heal", "activate"...); in the text, [POKEMON] and
    // the like are filled in, a leading "  " marks a minor line, and "#id" (or "#.key") means the text of another id
    // (or key). Taken from pokemon-showdown's data/text (default.ts, moves.ts, abilities.ts and items.ts) without the
    // descriptions and the texts of older generations, and sorted by id then key, for a binary search.
    struct Text {
        std::string_view id;
        std::string_view key;
        std::string_view text;
    };
    constexpr Text texts[] = {
        {"abilityshield", "block", "  [POKEMON]'s Ability is protected by the effects of its Ability Shield!"},
        {"abilityshield", "name", "Ability Shield"},
        {"accuracy", "statName", "accuracy"},
        {"aftermath", "damage", "  [POKEMON] was hurt!"},
        {"aftermath", "name", "Aftermath"},
        {"afteryou", "activate", "  [TARGET] took the kind offer!"},
        {"afteryou", "name", "After You"},
        {"airballoon", "end", "  [POKEMON]'s Air Balloon popped!"},
        {"airballoon", "name", "Air Balloon"},
        {"airballoon", "start", "  [POKEMON] floats in the air with its Air Balloon!"},
        {"airlock", "name", "Air Lock"},
        {"airlock", "start", "  The effects of the weather disappeared."},
        {"angerpoint", "boost", "  [POKEMON] maxed its Attack!"},
        {"angerpoint", "name", "Anger Point"},
        {"anticipation", "activate", "  [POKEMON] shuddered!"},
        {"anticipation", "name", "Anticipation"},
        {"aquaring", "heal", "  A veil of water restored [POKEMON]'s HP!"},
        {"aquaring", "name", "Aqua Ring"},
        {"aquaring", "start", "  [POKEMON] surrounded itself with a veil of water!"},
        {"armortail", "block", "#damp"},
        {"armortail", "name", "Armor Tail"},
        {"aromatherapy", "activate", "  A soothing aroma wafted through the area!"},
        {"aromatherapy", "name", "Aromatherapy"},
        {"aromaveil", "block", "  [POKEMON] is protected by an aromatic veil!"},
        {"aromaveil", "name", "Aroma Veil"},
        {"asone", "name", "As One"},
        {"asone", "start", "  [POKEMON] has two Abilities!"},
        {"atk", "statName", "Attack"},
        {"atk", "statShortName", "Atk"},
        {"attract", "activate", "  [POKEMON] is in love with [TARGET]!"},
        {"attract", "cant", "[POKEMON] is immobilized by love!"},
        {"attract", "end", "  [POKEMON] got over its infatuation!"},
        {"attract", "endFromItem", "  [POKEMON] cured its infatuation using its [ITEM]!"},
        {"attract", "name", "Attract"},
        {"attract", "start", "  [POKEMON] fell in love!"},
        {"attract", "startFromItem", "  [POKEMON] fell in love because of the [ITEM]!"},
        {"aurabreak", "name", "Aura Break"},
        {"aurabreak", "start", "  [POKEMON] reversed all other Pokémon's auras!"},
        {"auroraveil", "end", "  [TEAM]'s Aurora Veil wore off!"},
        {"auroraveil", "name", "Aurora Veil"},
        {"auroraveil", "start", "  Aurora Veil made [TEAM] stronger against physical and special moves!"},
        {"autotomize", "name", "Autotomize"},
        {"autotomize", "start", "  [POKEMON] became nimble!"},
        {"axekick", "damage", "#crash"},
        {"axekick", "name", "Axe Kick"},
        {"baddreams", "damage", "  [POKEMON] is tormented!"},
        {"baddreams", "name", "Bad Dreams"},
        {"battlebond", "activate", "  [POKEMON] became fully charged due to its bond with its Trainer!"},
        {"battlebond", "name", "Battle Bond"},
        {"battlebond", "transform", "[POKEMON] became Ash-Greninja!"},
        {"beadsofruin", "name", "Beads of Ruin"},
        {"beadsofruin", "start", "  [POKEMON]'s Beads of Ruin weakened the Sp. Def of all surrounding Pokémon!"},
        {"beakblast", "name", "Beak Blast"},
        {"beakblast", "start", "  [POKEMON] started heating up its beak!"},
        {"beatup", "activate", "  [NAME]'s attack!"},
        {"beatup", "name", "Beat Up"},
        {"bellydrum", "boost", "  [POKEMON] cut its own HP and maximized its Attack!"},
        {"bellydrum", "name", "Belly Drum"},
        {"bestow", "name", "Bestow"},
        {"bestow", "takeItem", "  [SOURCE] gave [POKEMON] its [ITEM]!"},
        {"bide", "activate", "  [POKEMON] is storing energy!"},
        {"bide", "end", "  [POKEMON] unleashed its energy!"},
        {"bide", "name", "Bide"},
        {"bide", "start", "  [POKEMON] is storing energy!"},
        {"bind", "move", "#wrap"},
        {"bind", "name", "Bind"},
        {"bind", "start", "  [POKEMON] was squeezed by [SOURCE]!"},
        {"blacksludge", "heal", "  [POKEMON] restored a little HP using its Black Sludge!"},
        {"blacksludge", "name", "Black Sludge"},
        {"bounce", "name", "Bounce"},
        {"bounce", "prepare", "[POKEMON] sprang up!"},
        {"brickbreak", "activate", "  [POKEMON] shattered [TEAM]'s protections!"},
        {"brickbreak", "name", "Brick Break"},
        {"brn", "alreadyStarted", "  [POKEMON] is already burned!"},
        {"brn", "damage", "  [POKEMON] was hurt by its burn!"},
        {"brn", "end", "  [POKEMON]'s burn was healed!"},
        {"brn", "endFromItem", "  [POKEMON]'s [ITEM] healed its burn!"},
        {"brn", "start", "  [POKEMON] was burned!"},
        {"brn", "startFromItem", "  [POKEMON] was burned by the [ITEM]!"},
        {"bugbite", "name", "Bug Bite"},
        {"bugbite", "removeItem", "  [SOURCE] stole and ate its target's [ITEM]!"},
        {"burnup", "name", "Burn Up"},
        {"burnup", "typeChange", "  [POKEMON] burned itself out!"},
        {"celebrate", "activate", "  Congratulations, [TRAINER]!"},
        {"celebrate", "name", "Celebrate"},
        {"charge", "name", "Charge"},
        {"charge", "start", "  [POKEMON] began charging power!"},
        {"chillyreception", "name", "Chilly Reception"},
        {"chillyreception", "prepare", "  [POKEMON] is preparing to tell a chillingly bad joke!"},
        {"clamp", "move", "#wrap"},
        {"clamp", "name", "Clamp"},
        {"clamp", "start", "  [SOURCE] clamped down on [POKEMON]!"},
        {"clearamulet", "block", "  The effects of [POKEMON]'s Clear Amulet prevent its stats from being lowered!"},
        {"clearamulet", "name", "Clear Amulet"},
        {"cloudnine", "name", "Cloud Nine"},
        {"cloudnine", "start", "#airlock"},
        {"comatose", "name", "Comatose"},
        {"comatose", "start", "  [POKEMON] is drowsing!"},
        {"commander", "activate", "  [POKEMON] was swallowed by [TARGET] and became [TARGET]'s commander!"},
        {"commander", "name", "Commander"},
        {"confusion", "activate", "  [POKEMON] is confused!"},
        {"confusion", "alreadyStarted", "  [POKEMON] is already confused!"},
        {"confusion", "damage", "It hurt itself in its confusion!"},
        {"confusion", "end", "  [POKEMON] snapped out of its confusion!"},
        {"confusion", "endFromItem", "  [POKEMON]'s [ITEM] snapped it out of its confusion!"},
        {"confusion", "name", "Confusion"},
        {"confusion", "start", "  [POKEMON] became confused!"},
        {"confusion", "startFromFatigue", "  [POKEMON] became confused due to fatigue!"},
        {"conversion", "name", "Conversion"},
        {"conversion", "typeChange", "  Converted type to [SOURCE]'s!"},
        {"corrosivegas", "fail", "#healblock"},
        {"corrosivegas", "name", "Corrosive Gas"},
        {"corrosivegas", "removeItem", "  [SOURCE] corroded [POKEMON]'s [ITEM]!"},
        {"courtchange", "activate", "  [POKEMON] swapped the battle effects affecting each side of the field!"},
        {"courtchange", "name", "Court Change"},
        {"craftyshield", "block", "  Crafty Shield protected [POKEMON]!"},
        {"craftyshield", "name", "Crafty Shield"},
        {"craftyshield", "start", "  Crafty Shield protected [TEAM]!"},
        {"crash", "damage", "  [POKEMON] kept going and crashed!"},
        {"curse", "damage", "  [POKEMON] is afflicted by the curse!"},
        {"curse", "name", "Curse"},
        {"curse", "start", "  [SOURCE] cut its own HP and put a curse on [POKEMON]!"},
        {"custapberry", "activate", "  [POKEMON] can act faster than normal, thanks to its Custap Berry!"},
        {"custapberry", "name", "Custap Berry"},
        {"damp", "block", "  [SOURCE] cannot use [MOVE]!"},
        {"damp", "name", "Damp"},
        {"darkaura", "name", "Dark Aura"},
        {"darkaura", "start", "  [POKEMON] is radiating a dark aura!"},
        {"darkvoid", "fail", "But [POKEMON] can't use the move!"},
        {"darkvoid", "failWrongForme", "But [POKEMON] can't use it the way it is now!"},
        {"darkvoid", "name", "Dark Void"},
        {"dazzling", "block", "#damp"},
        {"dazzling", "name", "Dazzling"},
        {"def", "statName", "Defense"},
        {"def", "statShortName", "Def"},
        {"default", "abilityActivation", "[[POKEMON]'s [ABILITY]]"},
        {"default", "activate", "  ([EFFECT] activated!)"},
        {"default", "activateItem", "  ([POKEMON] used its [ITEM]!)"},
        {"default", "activateWeaken", "  The [ITEM] weakened the damage to [POKEMON]!"},
        {"default", "addItem", "  [POKEMON] obtained one [ITEM]."},
        {"default", "boost", "  [POKEMON]'s [STAT] rose!"},
        {"default", "boost0", "  [POKEMON]'s [STAT] won't go any higher!"},
        {"default", "boost2", "  [POKEMON]'s [STAT] rose sharply!"},
        {"default", "boost2FromItem", "  The [ITEM] sharply raised [POKEMON]'s [STAT]!"},
        {"default", "boost2FromZEffect", "  [POKEMON] boosted its [STAT] sharply using its Z-Power!"},
        {"default", "boost3", "  [POKEMON]'s [STAT] rose drastically!"},
        {"default", "boost3FromItem", "  The [ITEM] drastically raised [POKEMON]'s [STAT]!"},
        {"default", "boost3FromZEffect", "  [POKEMON] boosted its [STAT] drastically using its Z-Power!"},
        {"default", "boostFromItem", "  The [ITEM] raised [POKEMON]'s [STAT]!"},
        {"default", "boostFromZEffect", "  [POKEMON] boosted its [STAT] using its Z-Power!"},
        {"default", "boostMultipleFromZEffect", "  [POKEMON] boosted its stats using its Z-Power!"},
        {"default", "canDynamax", "  [TRAINER] can dynamax now!"},
        {"default", "canDynamaxOwn", "  Dynamax Energy gathered around [TRAINER]!"},
        {"default", "cant", "[POKEMON] can't use [MOVE]!"},
        {"default", "cantNoMove", "[POKEMON] can't move!"},
        {"default", "center", "  Automatic center!"},
        {"default", "changeAbility", "  [POKEMON] acquired [ABILITY]!"},
        {"default", "clearAllBoost", "  All stat changes were eliminated!"},
        {"default", "clearBoost", "  [POKEMON]'s stat changes were removed!"},
        {"default", "clearBoostFromZEffect", "  [POKEMON] returned its decreased stats to normal using its Z-Power!"},
        {"default", "combine", "  The two moves have become one! It's a combined move!"},
        {"default", "copyBoost", "  [POKEMON] copied [TARGET]'s stat changes!"},
        {"default", "crit", "  A critical hit!"},
        {"default", "critSpread", "  A critical hit on [POKEMON]!"},
        {"default", "damage", "  ([POKEMON] was hurt!)"},
        {"default", "damageFromItem", "  [POKEMON] was hurt by its [ITEM]!"},
        {"default", "damageFromPartialTrapping", "  [POKEMON] is hurt by [MOVE]!"},
        {"default", "damageFromPokemon", "  [POKEMON] was hurt by [SOURCE]'s [ITEM]!"},
        {"default", "damagePercentage", "  ([POKEMON] lost [PERCENTAGE] of its health!)"},
        {"default", "drag", "[FULLNAME] was dragged out!"},
        {"default", "eatItem", "  ([POKEMON] ate its [ITEM]!)"},
        {"default", "eatItemWeaken", "  The [ITEM] weakened damage to [POKEMON]!"},
        {"default", "end", "  [POKEMON] was freed from [EFFECT]!"},
        {"default", "endFieldEffect", "  ([EFFECT] ended!)"},
        {"default", "endTeamEffect", "  ([EFFECT] ended on [TEAM]!)"},
        {"default", "extremelyEffective", "  It's extremely effective!"},
        {"default", "extremelyEffectiveSpread", "  It's extremely effective on [POKEMON]!"},
        {"default", "fail", "  But it failed!"},
        {"default", "faint", "[POKEMON] fainted!"},
        {"default", "heal", "  [POKEMON] had its HP restored."},
        {"default", "healFromEffect", "  [POKEMON] restored HP using its [EFFECT]!"},
        {"default", "healFromZEffect", "  [POKEMON] restored its HP using its Z-Power!"},
        {"default", "hitCount", "  The Pokémon was hit [NUMBER] times!"},
        {"default", "hitCountSingular", "  The Pokémon was hit 1 time!"},
        {"default", "immune", "  It doesn't affect [POKEMON]..."},
        {"default", "immuneNoPokemon", "  It had no effect!"},
        {"default", "immuneOHKO", "  [POKEMON] is unaffected!"},
        {"default", "invertBoost", "  [POKEMON]'s stat changes were inverted!"},
        {"default", "mega", "  [POKEMON]'s [ITEM] is reacting to the Key Stone!"},
        {"default", "megaGen6", "  [POKEMON]'s [ITEM] is reacting to [TRAINER]'s Mega Bracelet!"},
        {"default", "megaNoItem", "  [POKEMON] is reacting to [TRAINER]'s Key Stone!"},
        {"default", "miss", "  [POKEMON] avoided the attack!"},
        {"default", "missNoPokemon", "  [SOURCE]'s attack missed!"},
        {"default", "mostlyIneffective", "  It's mostly ineffective..."},
        {"default", "mostlyIneffectiveSpread", "  It's mostly ineffective on [POKEMON]."},
        {"default", "move", "[POKEMON] used **[MOVE]**!"},
        {"default", "noTarget", "  But there was no target..."},
        {"default", "ohko", "  It's a one-hit KO!"},
        {"default", "opposingParty", "the opposing Pokémon"},
        {"default", "opposingPokemon", "the opposing [NICKNAME]"},
        {"default", "opposingTeam", "the opposing team"},
        {"default", "party", "your ally Pokémon"},
        {"default", "pokemon", "[NICKNAME]"},
        {"default", "primal", "[POKEMON]'s Primal Reversion! It reverted to its primal state!"},
        {"default", "removeItem", "  [POKEMON] lost its [ITEM]!"},
        {"default", "resisted", "  It's not very effective..."},
        {"default", "resistedSpread", "  It's not very effective on [POKEMON]."},
        {"default", "start", "  ([EFFECT] started on [POKEMON]!)"},
        {"default", "startBattle", "Battle started between [TRAINER] and [TRAINER]!"},
        {"default", "startFieldEffect", "  ([EFFECT] started!)"},
        {"default", "startTeamEffect", "  ([EFFECT] started on [TEAM]!)"},
        {"default", "superEffective", "  It's super effective!"},
        {"default", "superEffectiveSpread", "  It's super effective on [POKEMON]!"},
        {"default", "swap", "[POKEMON] and [TARGET] switched places!"},
        {"default", "swapBoost", "  [POKEMON] switched stat changes with its target!"},
        {"default", "swapCenter", "[POKEMON] moved to the center!"},
        {"default", "swapDefensiveBoost",
         "  [POKEMON] switched all changes to its Defense and Sp. Def with its target!"},
        {"default", "swapOffensiveBoost",
         "  [POKEMON] switched all changes to its Attack and Sp. Atk with its target!"},
        {"default", "switchIn", "[TRAINER] sent out [FULLNAME]!"},
        {"default", "switchInOwn", "Go! [FULLNAME]!"},
        {"default", "switchOut", "[TRAINER] withdrew [NICKNAME]!"},
        {"default", "switchOutOwn", "[NICKNAME], come back!"},
        {"default", "takeItem", "  [POKEMON] stole [SOURCE]'s [ITEM]!"},
        {"default", "team", "your team"},
        {"default", "terastallize", "  [POKEMON] has Terastallized into the [TYPE]-type!"},
        {"default", "tieBattle", "Tie between [TRAINER] and [TRAINER]!"},
        {"default", "transform", "[POKEMON] transformed!"},
        {"default", "transformMega", "[POKEMON] has Mega Evolved into Mega [SPECIES]!"},
        {"default", "turn", "== Turn [NUMBER] =="},
        {"default", "typeAdd", "  [TYPE] type was added to [POKEMON]!"},
        {"default", "typeChange", "  [POKEMON]'s type changed to [TYPE]!"},
        {"default", "typeChangeFromEffect", "  [POKEMON]'s [EFFECT] made it the [TYPE] type!"},
        {"default", "unboost", "  [POKEMON]'s [STAT] fell!"},
        {"default", "unboost0", "  [POKEMON]'s [STAT] won't go any lower!"},
        {"default", "unboost2", "  [POKEMON]'s [STAT] fell harshly!"},
        {"default", "unboost2FromItem", "  The [ITEM] harshly lowered [POKEMON]'s [STAT]!"},
        {"default", "unboost3", "  [POKEMON]'s [STAT] fell severely!"},
        {"default", "unboost3FromItem", "  The [ITEM] drastically lowered [POKEMON]'s [STAT]!"},
        {"default", "unboostFromItem", "  The [ITEM] lowered [POKEMON]'s [STAT]!"},
        {"default", "useGem", "  The [ITEM] strengthened [POKEMON]'s power!"},
        {"default", "winBattle", "**[TRAINER]** won the battle!"},
        {"default", "zBroken", "  [POKEMON] couldn't fully protect itself and got hurt!"},
        {"default", "zEffect", "  [POKEMON] unleashes its full-force Z-Move!"},
        {"default", "zPower", "  [POKEMON] surrounded itself with its Z-Power!"},
        {"deltastream", "activate", "  The mysterious strong winds weakened the attack!"},
        {"deltastream", "block", "  The mysterious strong winds blow on regardless!"},
        {"deltastream", "end", "  The mysterious strong winds have dissipated!"},
        {"deltastream", "name", "Delta Stream"},
        {"deltastream", "start", "  Mysterious strong winds are protecting Flying-type Pokémon!"},
        {"deltastream", "weatherName", "Strong Winds"},
        {"desolateland", "block", "  The extremely harsh sunlight was not lessened at all!"},
        {"desolateland", "blockMove", "  The Water-type attack evaporated in the harsh sunlight!"},
        {"desolateland", "end", "  The extremely harsh sunlight faded."},
        {"desolateland", "name", "Desolate Land"},
        {"desolateland", "start", "  The sunlight turned extremely harsh!"},
        {"desolateland", "weatherName", "Intense Sun"},
        {"destinybond", "activate", "[POKEMON] took its attacker down with it!"},
        {"destinybond", "name", "Destiny Bond"},
        {"destinybond", "start", "  [POKEMON] is hoping to take its attacker down with it!"},
        {"dig", "name", "Dig"},
        {"dig", "prepare", "[POKEMON] burrowed its way under the ground!"},
        {"disable", "cant", "[POKEMON]'s [MOVE] is disabled!"},
        {"disable", "end", "  [POKEMON]'s move is no longer disabled!"},
        {"disable", "name", "Disable"},
        {"disable", "start", "  [POKEMON]'s [MOVE] was disabled!"},
        {"disguise", "block", "  Its disguise served it as a decoy!"},
        {"disguise", "name", "Disguise"},
        {"disguise", "transform", "[POKEMON]'s disguise was busted!"},
        {"dive", "name", "Dive"},
        {"dive", "prepare", "[POKEMON] hid underwater!"},
        {"doomdesire", "activate", "  [TARGET] took the Doom Desire attack!"},
        {"doomdesire", "name", "Doom Desire"},
        {"doomdesire", "start", "  [POKEMON] chose Doom Desire as its destiny!"},
        {"doubleshock", "name", "Double Shock"},
        {"doubleshock", "typeChange", "  [POKEMON] used up all of its electricity!"},
        {"dragonascent", "megaNoItem", "  [TRAINER]'s fervent wish has reached [POKEMON]!"},
        {"dragonascent", "name", "Dragon Ascent"},
        {"dragoncheer", "name", "Dragon Cheer"},
        {"dragoncheer", "start", "#focusenergy"},
        {"drain", "heal", "  [SOURCE] had its energy drained!"},
        {"dryskin", "damage", "  ([POKEMON] was hurt by its Dry Skin.)"},
        {"dryskin", "name", "Dry Skin"},
        {"dynamax", "block", "  The move was blocked by the power of Dynamax!"},
        {"dynamax", "end", "  ([POKEMON] returned to normal!)"},
        {"dynamax", "fail", "  [POKEMON] shook its head. It seems like it can't use this move..."},
        {"dynamax", "start", "  ([POKEMON]'s Dynamax!)"},
        {"eeriespell", "activate", "#spite"},
        {"eeriespell", "name", "Eerie Spell"},
        {"ejectbutton", "end", "  [POKEMON] is switched out with the Eject Button!"},
        {"ejectbutton", "name", "Eject Button"},
        {"ejectpack", "end", "  [POKEMON] is switched out by the Eject Pack!"},
        {"ejectpack", "name", "Eject Pack"},
        {"electricterrain", "block", "  [POKEMON] is protected by the Electric Terrain!"},
        {"electricterrain", "end", "  The electricity disappeared from the battlefield."},
        {"electricterrain", "name", "Electric Terrain"},
        {"electricterrain", "start", "  An electric current ran across the battlefield!"},
        {"electrify", "name", "Electrify"},
        {"electrify", "start", "  [POKEMON]'s moves have been electrified!"},
        {"electromorphosis", "name", "Electromorphosis"},
        {"electromorphosis", "start", "  Being hit by [MOVE] charged [POKEMON] with power!"},
        {"electroshot", "name", "Electro Shot"},
        {"electroshot", "prepare", "[POKEMON] absorbed electricity!"},
        {"embargo", "end", "  [POKEMON] can use items again!"},
        {"embargo", "name", "Embargo"},
        {"embargo", "start", "  [POKEMON] can't use items anymore!"},
        {"embodyaspectcornerstone", "boost",
         "  The Cornerstone Mask worn by [POKEMON] shone brilliantly, and [POKEMON]'s Defense rose!"},
        {"embodyaspectcornerstone", "name", "Embody Aspect (Cornerstone)"},
        {"embodyaspecthearthflame", "boost",
         "  The Hearthflame Mask worn by [POKEMON] shone brilliantly, and [POKEMON]'s Attack rose!"},
        {"embodyaspecthearthflame", "name", "Embody Aspect (Hearthflame)"},
        {"embodyaspectteal", "boost",
         "  The Teal Mask worn by [POKEMON] shone brilliantly, and [POKEMON]'s Speed rose!"},
        {"embodyaspectteal", "name", "Embody Aspect (Teal)"},
        {"embodyaspectwellspring", "boost",
         "  The Wellspring Mask worn by [POKEMON] shone brilliantly, and [POKEMON]'s Sp. Def rose!"},
        {"embodyaspectwellspring", "name", "Embody Aspect (Wellspring)"},
        {"encore", "end", "  [POKEMON]'s encore ended!"},
        {"encore", "name", "Encore"},
        {"encore", "start", "  [POKEMON] must do an encore!"},
        {"endure", "activate", "  [POKEMON] endured the hit!"},
        {"endure", "name", "Endure"},
        {"endure", "start", "  [POKEMON] braced itself!"},
        {"evasion", "statName", "evasiveness"},
        {"fairyaura", "name", "Fairy Aura"},
        {"fairyaura", "start", "  [POKEMON] is radiating a fairy aura!"},
        {"fairylock", "activate", "  No one will be able to run away during the next turn!"},
        {"fairylock", "name", "Fairy Lock"},
        {"feint", "activate", "  [TARGET] fell for the feint!"},
        {"feint", "name", "Feint"},
        {"ficklebeam", "activate", "  [POKEMON] is going all out for this attack!"},
        {"ficklebeam", "name", "Fickle Beam"},
        {"firepledge", "activate", "#waterpledge"},
        {"firepledge", "damage", "  [POKEMON] was hurt by the sea of fire!"},
        {"firepledge", "end", "  The sea of fire around [TEAM] disappeared!"},
        {"firepledge", "name", "Fire Pledge"},
        {"firepledge", "start", "  A sea of fire enveloped [TEAM]!"},
        {"firespin", "move", "#wrap"},
        {"firespin", "name", "Fire Spin"},
        {"firespin", "start", "  [POKEMON] became trapped in the fiery vortex!"},
        {"flameburst", "damage", "  The bursting flame hit [POKEMON]!"},
        {"flameburst", "name", "Flame Burst"},
        {"flashfire", "name", "Flash Fire"},
        {"flashfire", "start", "  The power of [POKEMON]'s Fire-type moves rose!"},
        {"flinch", "cant", "[POKEMON] flinched and couldn't move!"},
        {"fling", "name", "Fling"},
        {"fling", "removeItem", "  [POKEMON] flung its [ITEM]!"},
        {"flipturn", "name", "Flip Turn"},
        {"flipturn", "switchOut", "#uturn"},
        {"flowerveil", "block", "  [POKEMON] surrounded itself with a veil of petals!"},
        {"flowerveil", "name", "Flower Veil"},
        {"fly", "name", "Fly"},
        {"fly", "prepare", "[POKEMON] flew up high!"},
        {"focusband", "activate", "  [POKEMON] hung on using its Focus Band!"},
        {"focusband", "name", "Focus Band"},
        {"focusenergy", "name", "Focus Energy"},
        {"focusenergy", "start", "  [POKEMON] is getting pumped!"},
        {"focusenergy", "startFromItem", "  [POKEMON] used the [ITEM] to get pumped!"},
        {"focusenergy", "startFromZEffect", "  [POKEMON] boosted its critical-hit ratio using its Z-Power!"},
        {"focuspunch", "cant", "[POKEMON] lost its focus and couldn't move!"},
        {"focuspunch", "name", "Focus Punch"},
        {"focuspunch", "start", "  [POKEMON] is tightening its focus!"},
        {"focussash", "end", "  [POKEMON] hung on using its Focus Sash!"},
        {"focussash", "name", "Focus Sash"},
        {"followme", "name", "Follow Me"},
        {"followme", "start", "  [POKEMON] became the center of attention!"},
        {"followme", "startFromZEffect", "  [POKEMON] became the center of attention!"},
        {"foresight", "name", "Foresight"},
        {"foresight", "start", "  [POKEMON] was identified!"},
        {"forewarn", "activate", "  [TARGET]'s [MOVE] was revealed!"},
        {"forewarn", "activateNoTarget", "  [POKEMON]'s Forewarn alerted it to [MOVE]!"},
        {"forewarn", "name", "Forewarn"},
        {"freezeshock", "name", "Freeze Shock"},
        {"freezeshock", "prepare", "  [POKEMON] became cloaked in a freezing light!"},
        {"frisk", "activate", "  [POKEMON] frisked [TARGET] and found its [ITEM]!"},
        {"frisk", "activateNoTarget", "  [POKEMON] frisked its target and found one [ITEM]!"},
        {"frisk", "name", "Frisk"},
        {"frz", "alreadyStarted", "  [POKEMON] is already frozen solid!"},
        {"frz", "cant", "[POKEMON] is frozen solid!"},
        {"frz", "end", "  [POKEMON] thawed out!"},
        {"frz", "endFromItem", "  [POKEMON]'s [ITEM] defrosted it!"},
        {"frz", "endFromMove", "  [POKEMON]'s [MOVE] melted the ice!"},
        {"frz", "start", "  [POKEMON] was frozen solid!"},
        {"futuresight", "activate", "  [TARGET] took the Future Sight attack!"},
        {"futuresight", "name", "Future Sight"},
        {"futuresight", "start", "  [POKEMON] foresaw an attack!"},
        {"gastroacid", "name", "Gastro Acid"},
        {"gastroacid", "start", "  [POKEMON]'s Ability was suppressed!"},
        {"geomancy", "name", "Geomancy"},
        {"geomancy", "prepare", "[POKEMON] is absorbing power!"},
        {"gmaxcannonade", "damage", "  [POKEMON] is hurt by G-Max Cannonade’s vortex!"},
        {"gmaxcannonade", "name", "G-Max Cannonade"},
        {"gmaxcannonade", "start", "  [PARTY] got caught in the vortex of water!"},
        {"gmaxchistrike", "name", "G-Max Chi Strike"},
        {"gmaxchistrike", "start", "#focusenergy"},
        {"gmaxdepletion", "activate", "  [TARGET]'s PP was reduced!"},
        {"gmaxdepletion", "name", "G-Max Depletion"},
        {"gmaxsteelsurge", "damage", "  The sharp steel bit into [POKEMON]!"},
        {"gmaxsteelsurge", "end", "  The pieces of steel surrounding [PARTY] disappeared!"},
        {"gmaxsteelsurge", "name", "G-Max Steelsurge"},
        {"gmaxsteelsurge", "start", "  Sharp-pointed pieces of steel started floating around [PARTY]!"},
        {"gmaxvinelash", "damage", "  [POKEMON] is hurt by G-Max Vine Lash’s ferocious beating!"},
        {"gmaxvinelash", "name", "G-Max Vine Lash"},
        {"gmaxvinelash", "start", "  [PARTY] got trapped with vines!"},
        {"gmaxvolcalith", "damage", "  [POKEMON] is hurt by the rocks thrown out by G-Max Volcalith!"},
        {"gmaxvolcalith", "name", "G-Max Volcalith"},
        {"gmaxvolcalith", "start", "  [PARTY] became surrounded by rocks!"},
        {"gmaxwildfire", "damage", "  [POKEMON] is burning up within G-Max Wildfire’s flames!"},
        {"gmaxwildfire", "name", "G-Max Wildfire"},
        {"gmaxwildfire", "start", "  [PARTY] were surrounded by fire!"},
        {"grasspledge", "activate", "#waterpledge"},
        {"grasspledge", "end", "  The swamp around [TEAM] disappeared!"},
        {"grasspledge", "name", "Grass Pledge"},
        {"grasspledge", "start", "  A swamp enveloped [TEAM]!"},
        {"grassyterrain", "end", "  The grass disappeared from the battlefield."},
        {"grassyterrain", "heal", "  [POKEMON]'s HP was restored."},
        {"grassyterrain", "name", "Grassy Terrain"},
        {"grassyterrain", "start", "  Grass grew to cover the battlefield!"},
        {"gravity", "activate", "[POKEMON] fell from the sky due to the gravity!"},
        {"gravity", "cant", "[POKEMON] can't use [MOVE] because of gravity!"},
        {"gravity", "end", "  Gravity returned to normal!"},
        {"gravity", "name", "Gravity"},
        {"gravity", "start", "  Gravity intensified!"},
        {"grudge", "activate", "  [POKEMON]'s [MOVE] lost all of its PP due to the grudge!"},
        {"grudge", "name", "Grudge"},
        {"grudge", "start", "[POKEMON] wants its target to bear a grudge!"},
        {"guardsplit", "activate", "  [POKEMON] shared its guard with the target!"},
        {"guardsplit", "name", "Guard Split"},
        {"hadronengine", "activate", "  [POKEMON] used the Electric Terrain to energize its futuristic engine!"},
        {"hadronengine", "name", "Hadron Engine"},
        {"hadronengine", "start",
         "  [POKEMON] turned the ground into Electric Terrain, energizing its futuristic engine!"},
        {"hail", "damage", "  [POKEMON] is buffeted by the hail!"},
        {"hail", "end", "  The hail stopped."},
        {"hail", "name", "Hail"},
        {"hail", "start", "  It started to hail!"},
        {"hail", "upkeep", "  (The hail is crashing down.)"},
        {"hail", "weatherName", "Hail"},
        {"happyhour", "activate", "  Everyone is caught up in the happy atmosphere!"},
        {"happyhour", "name", "Happy Hour"},
        {"harvest", "addItem", "  [POKEMON] harvested one [ITEM]!"},
        {"harvest", "name", "Harvest"},
        {"haze", "activate", "  All STATUS changes are eliminated!"},
        {"haze", "name", "Haze"},
        {"heal", "fail", "  [POKEMON]'s HP is full!"},
        {"healbell", "activate", "  A bell chimed!"},
        {"healbell", "name", "Heal Bell"},
        {"healblock", "cant", "[POKEMON] is prevented from healing, so it can't use [MOVE]!"},
        {"healblock", "end", "  [POKEMON] is no longer prevented from healing!"},
        {"healblock", "fail", "  But it failed to affect [POKEMON]!"},
        {"healblock", "name", "Heal Block"},
        {"healblock", "start", "  [POKEMON] was prevented from healing!"},
        {"healingwish", "heal", "  The healing wish came true for [POKEMON]!"},
        {"healingwish", "name", "Healing Wish"},
        {"healreplacement", "activate", "  [POKEMON] will restore its replacement's HP using its Z-Power!"},
        {"helpinghand", "name", "Helping Hand"},
        {"helpinghand", "start", "  [SOURCE] is ready to help [POKEMON]!"},
        {"highjumpkick", "damage", "#crash"},
        {"highjumpkick", "name", "High Jump Kick"},
        {"hospitality", "heal", "  [POKEMON] drank down all the matcha that [SOURCE] made!"},
        {"hospitality", "name", "Hospitality"},
        {"hp", "statName", "HP"},
        {"hp", "statShortName", "HP"},
        {"hyperspacefury", "activate", "#shadowforce"},
        {"hyperspacefury", "fail", "#darkvoid"},
        {"hyperspacefury", "name", "Hyperspace Fury"},
        {"hyperspacehole", "activate", "#shadowforce"},
        {"hyperspacehole", "name", "Hyperspace Hole"},
        {"iceburn", "name", "Ice Burn"},
        {"iceburn", "prepare", "  [POKEMON] became cloaked in freezing air!"},
        {"illusion", "end", "  [POKEMON]'s illusion wore off!"},
        {"illusion", "name", "Illusion"},
        {"imprison", "cant", "[POKEMON] can't use its sealed [MOVE]!"},
        {"imprison", "name", "Imprison"},
        {"imprison", "start", "  [POKEMON] sealed any moves its target shares with it!"},
        {"incinerate", "name", "Incinerate"},
        {"incinerate", "removeItem", "  [POKEMON]'s [ITEM] was burned up!"},
        {"infestation", "name", "Infestation"},
        {"infestation", "start", "  [POKEMON] has been afflicted with an infestation by [SOURCE]!"},
        {"ingrain", "block", "  [POKEMON] is anchored in place with its roots!"},
        {"ingrain", "heal", "  [POKEMON] absorbed nutrients with its roots!"},
        {"ingrain", "name", "Ingrain"},
        {"ingrain", "start", "  [POKEMON] planted its roots!"},
        {"innardsout", "damage", "#aftermath"},
        {"innardsout", "name", "Innards Out"},
        {"instruct", "activate", "  [TARGET] followed [POKEMON]'s instructions!"},
        {"instruct", "name", "Instruct"},
        {"iondeluge", "activate", "  A deluge of ions showers the battlefield!"},
        {"iondeluge", "name", "Ion Deluge"},
        {"ironbarbs", "damage", "#roughskin"},
        {"ironbarbs", "name", "Iron Barbs"},
        {"jumpkick", "damage", "#crash"},
        {"jumpkick", "name", "Jump Kick"},
        {"knockoff", "name", "Knock Off"},
        {"knockoff", "removeItem", "  [SOURCE] knocked off [POKEMON]'s [ITEM]!"},
        {"laserfocus", "name", "Laser Focus"},
        {"laserfocus", "start", "  [POKEMON] concentrated intensely!"},
        {"leechseed", "damage", "  [POKEMON]'s health is sapped by Leech Seed!"},
        {"leechseed", "end", "  [POKEMON] was freed from Leech Seed!"},
        {"leechseed", "name", "Leech Seed"},
        {"leechseed", "start", "  [POKEMON] was seeded!"},
        {"leftovers", "heal", "  [POKEMON] restored a little HP using its Leftovers!"},
        {"leftovers", "name", "Leftovers"},
        {"leppaberry", "activate", "  [POKEMON] restored PP to its move [MOVE] using its Leppa Berry!"},
        {"leppaberry", "name", "Leppa Berry"},
        {"lifeorb", "damage", "  [POKEMON] lost some of its HP!"},
        {"lifeorb", "name", "Life Orb"},
        {"lightningrod", "activate", "  [POKEMON] took the attack!"},
        {"lightningrod", "name", "Lightning Rod"},
        {"lightscreen", "end", "  [TEAM]'s Light Screen wore off!"},
        {"lightscreen", "name", "Light Screen"},
        {"lightscreen", "start", "  Light Screen made [TEAM] stronger against special moves!"},
        {"lingeringaroma", "changeAbility", "  A lingering aroma clings to [TARGET]!"},
        {"lingeringaroma", "name", "Lingering Aroma"},
        {"liquidooze", "damage", "  [POKEMON] sucked up the liquid ooze!"},
        {"liquidooze", "name", "Liquid Ooze"},
        {"lockon", "name", "Lock-On"},
        {"lockon", "start", "  [SOURCE] took aim at [POKEMON]!"},
        {"luckychant", "end", "  [TEAM]'s Lucky Chant wore off!"},
        {"luckychant", "name", "Lucky Chant"},
        {"luckychant", "start", "  Lucky Chant shielded [TEAM] from critical hits!"},
        {"lunardance", "heal", "  [POKEMON] became cloaked in mystical moonlight!"},
        {"lunardance", "name", "Lunar Dance"},
        {"magicbounce", "move", "#magiccoat"},
        {"magicbounce", "name", "Magic Bounce"},
        {"magiccoat", "move", "[POKEMON] bounced the [MOVE] back!"},
        {"magiccoat", "name", "Magic Coat"},
        {"magiccoat", "start", "  [POKEMON] shrouded itself with Magic Coat!"},
        {"magicroom", "end", "  Magic Room wore off, and held items' effects returned to normal!"},
        {"magicroom", "name", "Magic Room"},
        {"magicroom", "start", "  It created a bizarre area in which Pokémon's held items lose their effects!"},
        {"magmastorm", "name", "Magma Storm"},
        {"magmastorm", "start", "  [POKEMON] became trapped by swirling magma!"},
        {"magnetrise", "end", "  [POKEMON]'s electromagnetism wore off!"},
        {"magnetrise", "name", "Magnet Rise"},
        {"magnetrise", "start", "  [POKEMON] levitated with electromagnetism!"},
        {"magnitude", "activate", "  Magnitude [NUMBER]!"},
        {"magnitude", "name", "Magnitude"},
        {"makeitrain", "activate", "#payday"},
        {"makeitrain", "name", "Make It Rain"},
        {"matblock", "block", "  [MOVE] was blocked by the kicked-up mat!"},
        {"matblock", "name", "Mat Block"},
        {"matblock", "start", "  [POKEMON] intends to flip up a mat and block incoming attacks!"},
        {"maxguard", "activate", "  [POKEMON] protected itself!"},
        {"maxguard", "name", "Max Guard"},
        {"memento", "heal", "  [POKEMON]'s HP was restored by the Z-Power!"},
        {"memento", "name", "Memento"},
        {"meteorbeam", "name", "Meteor Beam"},
        {"meteorbeam", "prepare", "[POKEMON] is overflowing with space power!"},
        {"metronome", "move", "Waggling a finger let it use [MOVE]!"},
        {"metronome", "name", "Metronome"},
        {"mimic", "name", "Mimic"},
        {"mimic", "start", "  [POKEMON] learned [MOVE]!"},
        {"mimicry", "activate", "  [POKEMON] returned to its original type!"},
        {"mimicry", "name", "Mimicry"},
        {"mindblown", "damage", "  ([POKEMON] cut its own HP to power up its move!)"},
        {"mindblown", "name", "Mind Blown"},
        {"mindreader", "name", "Mind Reader"},
        {"mindreader", "start", "#lockon"},
        {"miracleeye", "name", "Miracle Eye"},
        {"miracleeye", "start", "#foresight"},
        {"mirrorherb", "activate", "  [POKEMON] used its Mirror Herb to mirror its opponent's stat changes!"},
        {"mirrorherb", "name", "Mirror Herb"},
        {"mist", "block", "  [POKEMON] is protected by the mist!"},
        {"mist", "end", "  [TEAM] is no longer protected by mist!"},
        {"mist", "name", "Mist"},
        {"mist", "start", "  [TEAM] became shrouded in mist!"},
        {"mistyterrain", "block", "  [POKEMON] surrounds itself with a protective mist!"},
        {"mistyterrain", "end", "  The mist disappeared from the battlefield."},
        {"mistyterrain", "name", "Misty Terrain"},
        {"mistyterrain", "start", "  Mist swirled around the battlefield!"},
        {"moldbreaker", "name", "Mold Breaker"},
        {"moldbreaker", "start", "  [POKEMON] breaks the mold!"},
        {"mudsport", "end", "  The effects of Mud Sport have faded."},
        {"mudsport", "name", "Mud Sport"},
        {"mudsport", "start", "  Electricity's power was weakened!"},
        {"mummy", "changeAbility", "  [TARGET]'s Ability became Mummy!"},
        {"mummy", "name", "Mummy"},
        {"mysteryberry", "activate", "  [POKEMON] restored PP to its [MOVE] move using Mystery Berry!"},
        {"mysteryberry", "name", "Mystery Berry"},
        {"naturalcure", "activate", "  ([POKEMON] is cured by its Natural Cure!)"},
        {"naturalcure", "name", "Natural Cure"},
        {"naturepower", "move", "Nature Power turned into [MOVE]!"},
        {"naturepower", "name", "Nature Power"},
        {"neutralizinggas", "end", "  The effects of the neutralizing gas wore off!"},
        {"neutralizinggas", "name", "Neutralizing Gas"},
        {"neutralizinggas", "start", "  Neutralizing gas filled the area!"},
        {"nightmare", "damage", "  [POKEMON] is locked in a nightmare!"},
        {"nightmare", "name", "Nightmare"},
        {"nightmare", "start", "  [POKEMON] began having a nightmare!"},
        {"nopp", "cant", "[POKEMON] used [MOVE]!\n  But there was no PP left for the move!"},
        {"noretreat", "name", "No Retreat"},
        {"noretreat", "start", "  [POKEMON] can no longer escape because it used No Retreat!"},
        {"octolock", "name", "Octolock"},
        {"octolock", "start", "  [POKEMON] can no longer escape because of Octolock!"},
        {"orichalcumpulse", "activate", "  [POKEMON] basked in the sunlight, sending its ancient pulse into a frenzy!"},
        {"orichalcumpulse", "name", "Orichalcum Pulse"},
        {"orichalcumpulse", "start", "  [POKEMON] turned the sunlight harsh, sending its ancient pulse into a frenzy!"},
        {"painsplit", "activate", "  The battlers shared their pain!"},
        {"painsplit", "name", "Pain Split"},
        {"par", "alreadyStarted", "  [POKEMON] is already paralyzed!"},
        {"par", "cant", "[POKEMON] is paralyzed! It can't move!"},
        {"par", "end", "  [POKEMON] was cured of paralysis!"},
        {"par", "endFromItem", "  [POKEMON]'s [ITEM] cured its paralysis!"},
        {"par", "start", "  [POKEMON] is paralyzed! It may be unable to move!"},
        {"partingshot", "heal", "#memento"},
        {"partingshot", "name", "Parting Shot"},
        {"partingshot", "switchOut", "#uturn"},
        {"payday", "activate", "  Coins were scattered everywhere!"},
        {"payday", "name", "Pay Day"},
        {"perishbody", "name", "Perish Body"},
        {"perishbody", "start", "  Both Pokémon will faint in three turns!"},
        {"perishsong", "activate", "  [POKEMON]'s perish count fell to [NUMBER]."},
        {"perishsong", "name", "Perish Song"},
        {"perishsong", "start", "  All Pokémon that heard the song will faint in three turns!"},
        {"persistent", "activate", "  [POKEMON] extends [MOVE] by 2 turns!"},
        {"persistent", "name", "Persistent"},
        {"phantomforce", "activate", "#shadowforce"},
        {"phantomforce", "name", "Phantom Force"},
        {"phantomforce", "prepare", "#shadowforce"},
        {"pickup", "addItem", "#recycle"},
        {"pickup", "name", "Pickup"},
        {"pluck", "name", "Pluck"},
        {"pluck", "removeItem", "#bugbite"},
        {"poltergeist", "activate", "  [POKEMON] is about to be attacked by its [ITEM]!"},
        {"poltergeist", "name", "Poltergeist"},
        {"powder", "activate", "  When the flame touched the powder on the Pokémon, it exploded!"},
        {"powder", "name", "Powder"},
        {"powder", "start", "  [POKEMON] is covered in powder!"},
        {"powerconstruct", "activate", "  You sense the presence of many!"},
        {"powerconstruct", "name", "Power Construct"},
        {"powerconstruct", "transform", "[POKEMON] transformed into its Complete Forme!"},
        {"powerherb", "end", "  [POKEMON] became fully charged due to its Power Herb!"},
        {"powerherb", "name", "Power Herb"},
        {"powerofalchemy", "changeAbility", "#receiver"},
        {"powerofalchemy", "name", "Power of Alchemy"},
        {"powershift", "end", "#.start"},
        {"powershift", "name", "Power Shift"},
        {"powershift", "start", "  [POKEMON] swapped its offensive stats with its defensive stats!"},
        {"powersplit", "activate", "  [POKEMON] shared its power with the target!"},
        {"powersplit", "name", "Power Split"},
        {"powertrick", "end", "#.start"},
        {"powertrick", "name", "Power Trick"},
        {"powertrick", "start", "  [POKEMON] switched its Attack and Defense!"},
        {"pressure", "name", "Pressure"},
        {"pressure", "start", "  [POKEMON] is exerting its pressure!"},
        {"primordialsea", "block", "  There is no relief from this heavy rain!"},
        {"primordialsea", "blockMove", "  The Fire-type attack fizzled out in the heavy rain!"},
        {"primordialsea", "end", "  The heavy rain has lifted!"},
        {"primordialsea", "name", "Primordial Sea"},
        {"primordialsea", "start", "  A heavy rain began to fall!"},
        {"primordialsea", "weatherName", "Heavy Rain"},
        {"protect", "block", "  [POKEMON] protected itself!"},
        {"protect", "name", "Protect"},
        {"protect", "start", "  [POKEMON] protected itself!"},
        {"protectivepads", "block", "  [POKEMON] protected itself with its Protective Pads!"},
        {"protectivepads", "name", "Protective Pads"},
        {"protosynthesis", "activate", "  The harsh sunlight activated [POKEMON]'s Protosynthesis!"},
        {"protosynthesis", "activateFromItem", "  [POKEMON] used its Booster Energy to activate Protosynthesis!"},
        {"protosynthesis", "end", "  The effects of [POKEMON]'s Protosynthesis wore off!"},
        {"protosynthesis", "name", "Protosynthesis"},
        {"protosynthesis", "start", "  [POKEMON]'s [STAT] was heightened!"},
        {"psn", "alreadyStarted", "  [POKEMON] is already poisoned!"},
        {"psn", "damage", "  [POKEMON] was hurt by poison!"},
        {"psn", "end", "  [POKEMON] was cured of its poisoning!"},
        {"psn", "endFromItem", "  [POKEMON]'s [ITEM] cured its poison!"},
        {"psn", "start", "  [POKEMON] was poisoned!"},
        {"psychicterrain", "block", "  [POKEMON] is protected by the Psychic Terrain!"},
        {"psychicterrain", "end", "  The weirdness disappeared from the battlefield!"},
        {"psychicterrain", "name", "Psychic Terrain"},
        {"psychicterrain", "start", "  The battlefield got weird!"},
        {"pursuit", "activate", "  ([TARGET] is being withdrawn...)"},
        {"pursuit", "name", "Pursuit"},
        {"quarkdrive", "activate", "  The Electric Terrain activated [POKEMON]'s Quark Drive!"},
        {"quarkdrive", "activateFromItem", "  [POKEMON] used its Booster Energy to activate its Quark Drive!"},
        {"quarkdrive", "end", "  The effects of [POKEMON]'s Quark Drive wore off!"},
        {"quarkdrive", "name", "Quark Drive"},
        {"quarkdrive", "start", "  [POKEMON]'s [STAT] was heightened!"},
        {"quash", "activate", "  [TARGET]'s move was postponed!"},
        {"quash", "name", "Quash"},
        {"queenlymajesty", "block", "#damp"},
        {"queenlymajesty", "name", "Queenly Majesty"},
        {"quickclaw", "activate", "  [POKEMON] can act faster than normal, thanks to its Quick Claw!"},
        {"quickclaw", "name", "Quick Claw"},
        {"quickdraw", "activate", "  Quick Draw made [POKEMON] move faster!"},
        {"quickdraw", "name", "Quick Draw"},
        {"quickguard", "block", "  Quick Guard protected [POKEMON]!"},
        {"quickguard", "name", "Quick Guard"},
        {"quickguard", "start", "  Quick Guard protected [TEAM]!"},
        {"ragepowder", "name", "Rage Powder"},
        {"ragepowder", "start", "#followme"},
        {"ragepowder", "startFromZEffect", "#followme"},
        {"ragingbull", "activate", "  [POKEMON] shattered [TEAM]'s protections!"},
        {"ragingbull", "name", "Raging Bull"},
        {"raindance", "end", "  The rain stopped."},
        {"raindance", "name", "Rain Dance"},
        {"raindance", "start", "  It started to rain!"},
        {"raindance", "upkeep", "  (Rain continues to fall.)"},
        {"raindance", "weatherName", "Rain"},
        {"razorwind", "name", "Razor Wind"},
        {"razorwind", "prepare", "  [POKEMON] whipped up a whirlwind!"},
        {"rebound", "move", "#magiccoat"},
        {"rebound", "name", "Rebound"},
        {"receiver", "changeAbility", "  [SOURCE]'s [ABILITY] was taken over!"},
        {"receiver", "name", "Receiver"},
        {"recharge", "cant", "[POKEMON] must recharge!"},
        {"recoil", "damage", "  [POKEMON] was damaged by the recoil!"},
        {"recycle", "addItem", "  [POKEMON] found one [ITEM]!"},
        {"recycle", "name", "Recycle"},
        {"redcard", "end", "  [POKEMON] held up its Red Card against [TARGET]!"},
        {"redcard", "name", "Red Card"},
        {"reflect", "end", "  [TEAM]'s Reflect wore off!"},
        {"reflect", "name", "Reflect"},
        {"reflect", "start", "  Reflect made [TEAM] stronger against physical moves!"},
        {"reflecttype", "name", "Reflect Type"},
        {"reflecttype", "typeChange", "  [POKEMON]'s type became the same as [SOURCE]'s type!"},
        {"revivalblessing", "heal", "  [POKEMON] was revived and is ready to fight again!"},
        {"revivalblessing", "name", "Revival Blessing"},
        {"rockyhelmet", "damage", "  [POKEMON] was hurt by the Rocky Helmet!"},
        {"rockyhelmet", "name", "Rocky Helmet"},
        {"roleplay", "changeAbility", "  [POKEMON] copied [SOURCE]'s [ABILITY] Ability!"},
        {"roleplay", "name", "Role Play"},
        {"roost", "name", "Roost"},
        {"roost", "start", "  ([POKEMON] loses Flying type this turn.)"},
        {"roughskin", "damage", "  [POKEMON] was hurt!"},
        {"roughskin", "name", "Rough Skin"},
        {"safeguard", "block", "  [POKEMON] is protected by Safeguard!"},
        {"safeguard", "end", "  [TEAM] is no longer protected by Safeguard!"},
        {"safeguard", "name", "Safeguard"},
        {"safeguard", "start", "  [TEAM] cloaked itself in a mystical veil!"},
        {"safetygoggles", "block", "  [POKEMON] is not affected by [MOVE] thanks to its Safety Goggles!"},
        {"safetygoggles", "name", "Safety Goggles"},
        {"saltcure", "damage", "  [POKEMON] is hurt by Salt Cure!"},
        {"saltcure", "name", "Salt Cure"},
        {"saltcure", "start", "  [POKEMON] is being salt cured!"},
        {"sandstorm", "damage", "  [POKEMON] is buffeted by the sandstorm!"},
        {"sandstorm", "end", "  The sandstorm subsided."},
        {"sandstorm", "name", "Sandstorm"},
        {"sandstorm", "start", "  A sandstorm kicked up!"},
        {"sandstorm", "upkeep", "  (The sandstorm is raging.)"},
        {"sandstorm", "weatherName", "Sandstorm"},
        {"sandtomb", "name", "Sand Tomb"},
        {"sandtomb", "start", "  [POKEMON] became trapped by the quicksand!"},
        {"schooling", "name", "Schooling"},
        {"schooling", "transform", "[POKEMON] formed a school!"},
        {"schooling", "transformEnd", "[POKEMON] stopped schooling!"},
        {"shadowforce", "activate", "  It broke through [TARGET]'s protection!"},
        {"shadowforce", "name", "Shadow Force"},
        {"shadowforce", "prepare", "[POKEMON] vanished instantly!"},
        {"shedtail", "alreadyStarted", "#substitute"},
        {"shedtail", "fail", "#substitute"},
        {"shedtail", "name", "Shed Tail"},
        {"shedtail", "start", "  [POKEMON] shed its tail to create a decoy!"},
        {"shellbell", "heal", "  [POKEMON] restored a little HP using its Shell Bell!"},
        {"shellbell", "name", "Shell Bell"},
        {"shelltrap", "cant", "[POKEMON]'s shell trap didn't work!"},
        {"shelltrap", "name", "Shell Trap"},
        {"shelltrap", "prepare", "  [POKEMON] set a shell trap!"},
        {"shelltrap", "start", "  [POKEMON] set a shell trap!"},
        {"shieldsdown", "name", "Shields Down"},
        {"shieldsdown", "transform", "Shields Down deactivated!\n([POKEMON] shielded itself.)"},
        {"shieldsdown", "transformEnd", "Shields Down activated!\n([POKEMON] stopped shielding itself.)"},
        {"sketch", "activate", "  [POKEMON] sketched [MOVE]!"},
        {"sketch", "name", "Sketch"},
        {"skillswap", "activate", "  [POKEMON] swapped Abilities with its target!"},
        {"skillswap", "name", "Skill Swap"},
        {"skullbash", "name", "Skull Bash"},
        {"skullbash", "prepare", "[POKEMON] tucked in its head!"},
        {"skyattack", "name", "Sky Attack"},
        {"skyattack", "prepare", "[POKEMON] became cloaked in a harsh light!"},
        {"skydrop", "end", "  [POKEMON] was freed from the Sky Drop!"},
        {"skydrop", "failSelect", "Sky Drop won't let [POKEMON] go!"},
        {"skydrop", "failTooHeavy", "  [POKEMON] is too heavy to be lifted!"},
        {"skydrop", "name", "Sky Drop"},
        {"skydrop", "prepare", "[POKEMON] took [TARGET] into the sky!"},
        {"slowstart", "end", "  [POKEMON] finally got its act together!"},
        {"slowstart", "name", "Slow Start"},
        {"slowstart", "start", "  [POKEMON] can't get it going!"},
        {"slp", "alreadyStarted", "  [POKEMON] is already asleep!"},
        {"slp", "cant", "[POKEMON] is fast asleep."},
        {"slp", "end", "  [POKEMON] woke up!"},
        {"slp", "endFromItem", "  [POKEMON]'s [ITEM] woke it up!"},
        {"slp", "start", "  [POKEMON] fell asleep!"},
        {"slp", "startFromRest", "  [POKEMON] slept and became healthy!"},
        {"smackdown", "name", "Smack Down"},
        {"smackdown", "start", "  [POKEMON] fell straight down!"},
        {"snaptrap", "name", "Snap Trap"},
        {"snaptrap", "start", "  [POKEMON] got trapped by a snap trap!"},
        {"snatch", "activate", "  [POKEMON] snatched [TARGET]'s move!"},
        {"snatch", "name", "Snatch"},
        {"snatch", "start", "  [POKEMON] is waiting for a target to make a move!"},
        {"snowscape", "end", "  The snow stopped."},
        {"snowscape", "name", "Snowscape"},
        {"snowscape", "start", "  It started to snow!"},
        {"snowscape", "upkeep", "  (The snow is falling down.)"},
        {"snowscape", "weatherName", "Snow"},
        {"solarbeam", "name", "Solar Beam"},
        {"solarbeam", "prepare", "  [POKEMON] absorbed light!"},
        {"solarblade", "name", "Solar Blade"},
        {"solarblade", "prepare", "#solarbeam"},
        {"spa", "statName", "Sp. Atk"},
        {"spa", "statShortName", "SpA"},
        {"spc", "statName", "Special"},
        {"spc", "statShortName", "Spc"},
        {"spd", "statName", "Sp. Def"},
        {"spd", "statShortName", "SpD"},
        {"spe", "statName", "Speed"},
        {"spe", "statShortName", "Spe"},
        {"spectralthief", "clearBoost", "  [SOURCE] stole the target's boosted stats!"},
        {"spectralthief", "name", "Spectral Thief"},
        {"speedswap", "activate", "  [POKEMON] switched Speed with its target!"},
        {"speedswap", "name", "Speed Swap"},
        {"spikes", "damage", "  [POKEMON] was hurt by the spikes!"},
        {"spikes", "end", "  The spikes disappeared from the ground around [TEAM]!"},
        {"spikes", "name", "Spikes"},
        {"spikes", "start", "  Spikes were scattered on the ground all around [TEAM]!"},
        {"spikyshield", "damage", "#roughskin"},
        {"spikyshield", "name", "Spiky Shield"},
        {"spite", "activate", "  It reduced the PP of [TARGET]'s [MOVE] by [NUMBER]!"},
        {"spite", "name", "Spite"},
        {"splash", "activate", "  But nothing happened!"},
        {"splash", "name", "Splash"},
        {"spotlight", "name", "Spotlight"},
        {"spotlight", "start", "#followme"},
        {"spotlight", "startFromZEffect", "#followme"},
        {"stancechange", "name", "Stance Change"},
        {"stancechange", "transform", "Changed to Blade Forme!"},
        {"stancechange", "transformEnd", "Changed to Shield Forme!"},
        {"stats", "statName", "stats"},
        {"stealthrock", "damage", "  Pointed stones dug into [POKEMON]!"},
        {"stealthrock", "end", "  The pointed stones disappeared from around [TEAM]!"},
        {"stealthrock", "name", "Stealth Rock"},
        {"stealthrock", "start", "  Pointed stones float in the air around [TEAM]!"},
        {"steelbeam", "damage", "#mindblown"},
        {"steelbeam", "name", "Steel Beam"},
        {"stickyhold", "block", "  [POKEMON]'s item cannot be removed!"},
        {"stickyhold", "name", "Sticky Hold"},
        {"stickyweb", "activate", "  [POKEMON] was caught in a sticky web!"},
        {"stickyweb", "end", "  The sticky web has disappeared from the ground around [TEAM]!"},
        {"stickyweb", "name", "Sticky Web"},
        {"stickyweb", "start", "  A sticky web has been laid out on the ground around [TEAM]!"},
        {"stockpile", "end", "  [POKEMON]'s stockpiled effect wore off!"},
        {"stockpile", "name", "Stockpile"},
        {"stockpile", "start", "  [POKEMON] stockpiled [NUMBER]!"},
        {"stormdrain", "activate", "#lightningrod"},
        {"stormdrain", "name", "Storm Drain"},
        {"struggle", "activate", "  [POKEMON] has no moves left!"},
        {"struggle", "name", "Struggle"},
        {"sturdy", "activate", "  [POKEMON] endured the hit!"},
        {"sturdy", "name", "Sturdy"},
        {"substitute", "activate", "  The substitute took damage for [POKEMON]!"},
        {"substitute", "alreadyStarted", "  [POKEMON] already has a substitute!"},
        {"substitute", "end", "  [POKEMON]'s substitute faded!"},
        {"substitute", "fail", "  But it does not have enough HP left to make a substitute!"},
        {"substitute", "name", "Substitute"},
        {"substitute", "start", "  [POKEMON] put in a substitute!"},
        {"suctioncups", "block", "  [POKEMON] is anchored in place with its suction cups!"},
        {"suctioncups", "name", "Suction Cups"},
        {"sunnyday", "end", "  The harsh sunlight faded."},
        {"sunnyday", "name", "Sunny Day"},
        {"sunnyday", "start", "  The sunlight turned harsh!"},
        {"sunnyday", "upkeep", "  (The sunlight is strong.)"},
        {"sunnyday", "weatherName", "Sun"},
        {"supercellslam", "damage", "#crash"},
        {"supercellslam", "name", "Supercell Slam"},
        {"supersweetsyrup", "name", "Supersweet Syrup"},
        {"supersweetsyrup", "start", "  A supersweet aroma is wafting from the syrup covering [POKEMON]!"},
        {"supremeoverlord", "activate", "  [POKEMON] gained strength from the fallen!"},
        {"supremeoverlord", "name", "Supreme Overlord"},
        {"sweetveil", "block", "  [POKEMON] can't fall asleep due to a veil of sweetness!"},
        {"sweetveil", "name", "Sweet Veil"},
        {"switcheroo", "activate", "#trick"},
        {"switcheroo", "name", "Switcheroo"},
        {"swordofruin", "name", "Sword of Ruin"},
        {"swordofruin", "start", "  [POKEMON]'s Sword of Ruin weakened the Defense of all surrounding Pokémon!"},
        {"symbiosis", "activate", "  [POKEMON] shared its [ITEM] with [TARGET]!"},
        {"symbiosis", "name", "Symbiosis"},
        {"syrupbomb", "name", "Syrup Bomb"},
        {"syrupbomb", "start", "  [POKEMON] got covered in sticky candy syrup!"},
        {"tabletsofruin", "name", "Tablets of Ruin"},
        {"tabletsofruin", "start", "  [POKEMON]'s Tablets of Ruin weakened the Attack of all surrounding Pokémon!"},
        {"tailwind", "end", "  [TEAM]'s Tailwind petered out!"},
        {"tailwind", "name", "Tailwind"},
        {"tailwind", "start", "  The Tailwind blew from behind [TEAM]!"},
        {"tarshot", "name", "Tar Shot"},
        {"tarshot", "start", "  [POKEMON] became weaker to fire!"},
        {"taunt", "cant", "[POKEMON] can't use [MOVE] after the taunt!"},
        {"taunt", "end", "  [POKEMON] shook off the taunt!"},
        {"taunt", "name", "Taunt"},
        {"taunt", "start", "  [POKEMON] fell for the taunt!"},
        {"teatime", "activate", "  It's teatime! Everyone dug in to their Berries!"},
        {"teatime", "fail", "  But nothing happened!"},
        {"teatime", "name", "Teatime"},
        {"telekinesis", "end", "  [POKEMON] was freed from the telekinesis!"},
        {"telekinesis", "name", "Telekinesis"},
        {"telekinesis", "start", "  [POKEMON] was hurled into the air!"},
        {"telepathy", "block", "  [POKEMON] can't be hit by attacks from its ally Pokémon!"},
        {"telepathy", "name", "Telepathy"},
        {"terashell", "activate", "  [POKEMON] made its shell gleam! It's distorting type matchups!"},
        {"terashell", "name", "Tera Shell"},
        {"terashift", "name", "Tera Shift"},
        {"terashift", "transform", "[POKEMON] transformed!"},
        {"teravolt", "name", "Teravolt"},
        {"teravolt", "start", "  [POKEMON] is radiating a bursting aura!"},
        {"throatchop", "cant", "The effects of Throat Chop prevent [POKEMON] from using certain moves!"},
        {"throatchop", "name", "Throat Chop"},
        {"thundercage", "name", "Thunder Cage"},
        {"thundercage", "start", "  [SOURCE] trapped [POKEMON]!"},
        {"tidyup", "activate", "  Tidying up complete!"},
        {"tidyup", "name", "Tidy Up"},
        {"torment", "end", "  [POKEMON] is no longer tormented!"},
        {"torment", "name", "Torment"},
        {"torment", "start", "  [POKEMON] was subjected to torment!"},
        {"tox", "alreadyStarted", "#psn"},
        {"tox", "damage", "#psn"},
        {"tox", "end", "#psn"},
        {"tox", "endFromItem", "#psn"},
        {"tox", "start", "  [POKEMON] was badly poisoned!"},
        {"tox", "startFromItem", "  [POKEMON] was badly poisoned by the [ITEM]!"},
        {"toxicspikes", "end", "  The poison spikes disappeared from the ground around [TEAM]!"},
        {"toxicspikes", "name", "Toxic Spikes"},
        {"toxicspikes", "start", "  Poison spikes were scattered on the ground all around [TEAM]!"},
        {"trace", "changeAbility", "  [POKEMON] traced [SOURCE]'s [ABILITY]!"},
        {"trace", "name", "Trace"},
        {"transform", "name", "Transform"},
        {"transform", "transform", "[POKEMON] transformed into [SPECIES]!"},
        {"trapped", "start", "  [POKEMON] can no longer escape!"},
        {"trick", "activate", "  [POKEMON] switched items with its target!"},
        {"trick", "name", "Trick"},
        {"trickroom", "end", "  The twisted dimensions returned to normal!"},
        {"trickroom", "name", "Trick Room"},
        {"trickroom", "start", "  [POKEMON] twisted the dimensions!"},
        {"truant", "cant", "[POKEMON] is loafing around!"},
        {"truant", "name", "Truant"},
        {"turboblaze", "name", "Turboblaze"},
        {"turboblaze", "start", "  [POKEMON] is radiating a blazing aura!"},
        {"ultranecroziumz", "activate", "[POKEMON] regained its true power through Ultra Burst!"},
        {"ultranecroziumz", "name", "Ultranecrozium Z"},
        {"ultranecroziumz", "transform", "  Bright light is about to burst out of [POKEMON]!"},
        {"unboost", "fail", "  [POKEMON]'s stats were not lowered!"},
        {"unboost", "failSingular", "  [POKEMON]'s [STAT] was not lowered!"},
        {"unnerve", "name", "Unnerve"},
        {"unnerve", "start", "  [TEAM] is too nervous to eat Berries!"},
        {"uproar", "block", "  But the uproar kept [POKEMON] awake!"},
        {"uproar", "blockSelf", "  [POKEMON] can't sleep in an uproar!"},
        {"uproar", "end", "  [POKEMON] calmed down."},
        {"uproar", "name", "Uproar"},
        {"uproar", "start", "  [POKEMON] caused an uproar!"},
        {"uproar", "upkeep", "  [POKEMON] is making an uproar!"},
        {"uturn", "name", "U-turn"},
        {"uturn", "switchOut", "[POKEMON] went back to [TRAINER]!"},
        {"vesselofruin", "name", "Vessel of Ruin"},
        {"vesselofruin", "start", "  [POKEMON]'s Vessel of Ruin weakened the Sp. Atk of all surrounding Pokémon!"},
        {"voltswitch", "name", "Volt Switch"},
        {"voltswitch", "switchOut", "#uturn"},
        {"wanderingspirit", "activate", "#skillswap"},
        {"wanderingspirit", "name", "Wandering Spirit"},
        {"waterpledge", "activate", "  [POKEMON] is waiting for [TARGET]'s move..."},
        {"waterpledge", "end", "  The rainbow on [TEAM]'s side disappeared!"},
        {"waterpledge", "name", "Water Pledge"},
        {"waterpledge", "start", "  A rainbow appeared in the sky on [TEAM]'s side!"},
        {"watersport", "end", "  The effects of Water Sport have faded."},
        {"watersport", "name", "Water Sport"},
        {"watersport", "start", "  Fire's power was weakened!"},
        {"weatherball", "move", "Breakneck Blitz turned into [MOVE] due to the weather!"},
        {"weatherball", "name", "Weather Ball"},
        {"whirlpool", "name", "Whirlpool"},
        {"whirlpool", "start", "  [POKEMON] became trapped in the vortex!"},
        {"whiteherb", "end", "  [POKEMON] returned its stats to normal using its White Herb!"},
        {"whiteherb", "name", "White Herb"},
        {"wideguard", "block", "  Wide Guard protected [POKEMON]!"},
        {"wideguard", "name", "Wide Guard"},
        {"wideguard", "start", "  Wide Guard protected [TEAM]!"},
        {"windpower", "name", "Wind Power"},
        {"windpower", "start", "#electromorphosis"},
        {"wish", "heal", "  [NICKNAME]'s wish came true!"},
        {"wish", "name", "Wish"},
        {"wonderroom", "end", "  Wonder Room wore off, and Defense and Sp. Def stats returned to normal!"},
        {"wonderroom", "name", "Wonder Room"},
        {"wonderroom", "start", "  It created a bizarre area in which Defense and Sp. Def stats are swapped!"},
        {"wrap", "move", "[POKEMON]'s attack continues!"},
        {"wrap", "name", "Wrap"},
        {"wrap", "start", "  [POKEMON] was wrapped by [SOURCE]!"},
        {"yawn", "name", "Yawn"},
        {"yawn", "start", "  [POKEMON] grew drowsy!"},
        {"zenmode", "name", "Zen Mode"},
        {"zenmode", "transform", "Zen Mode triggered!"},
        {"zenmode", "transformEnd", "Zen Mode ended!"},
        {"zerotohero", "activate", "  [POKEMON] underwent a heroic transformation!"},
        {"zerotohero", "name", "Zero to Hero"},
    };

    // A text of the table, or empty.
    std::string_view lookup(std::string_view id, std::string_view key) {
        static const bool sorted = std::ranges::is_sorted(texts, {}, [](const Text& t) {
            return std::pair(t.id, t.key);
        });
        if (!sorted) {
            for (const Text& t : texts) {
                if (t.id == id && t.key == key) {
                    return t.text;
                }
            }
            return {};
        }
        const auto it = std::ranges::lower_bound(texts, std::pair(id, key), {}, [](const Text& t) {
            return std::pair(t.id, t.key);
        });
        return it != std::end(texts) && it->id == id && it->key == key ? it->text : std::string_view();
    }

    // Showdown's ids: lowercase letters and digits only ("Leech Seed" is "leechseed").
    std::string to_id(std::string_view s) {
        std::string id;
        for (const char c : s) {
            if (c >= 'A' && c <= 'Z') {
                id += static_cast<char>(c - 'A' + 'a');
            } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                id += c;
            }
        }
        return id;
    }

    std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
            s.remove_suffix(1);
        }
        return s;
    }

    bool starts_with(std::string_view s, std::string_view prefix) {
        return s.substr(0, prefix.size()) == prefix;
    }

    // An effect's name without the kind before it: "move: Protect", "ability: Levitate" and "item: Leftovers" are
    // Protect, Levitate and Leftovers.
    std::string_view effect_name(std::string_view effect) {
        for (const std::string_view kind : {"move:", "ability:", "item:", "pokemon:"}) {
            if (starts_with(effect, kind)) {
                return trim(effect.substr(kind.size()));
            }
        }
        return trim(effect);
    }

    std::string effect_id(std::string_view effect) {
        return to_id(effect_name(effect));
    }

    std::vector<std::string> split(std::string_view s, char separator) {
        std::vector<std::string> parts;
        while (!s.empty()) {
            const auto at = s.find(separator);
            if (const auto part = trim(s.substr(0, at)); !part.empty()) {
                parts.emplace_back(part);
            }
            s.remove_prefix(at == std::string_view::npos ? s.size() : at + 1);
        }
        return parts;
    }

    int to_int(std::string_view s, int fallback = 0) {
        s = trim(s);
        int value = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        return s.empty() || ec != std::errc {} || ptr != s.data() + s.size() ? fallback : value;
    }

    // Fills a text's [KEY]s with the values given, in one pass (so that a name with brackets in it stays as it is).
    // The ones not given are left as they are.
    std::string fill(std::string_view text, std::initializer_list<std::pair<std::string_view, std::string_view>> with) {
        std::string out;
        while (!text.empty()) {
            const auto open = text.find('[');
            out += text.substr(0, open);
            if (open == std::string_view::npos) {
                break;
            }
            text.remove_prefix(open);
            const auto close = text.find(']');
            bool done = false;
            if (close != std::string_view::npos) {
                for (const auto& [key, value] : with) {
                    if (text.substr(0, close + 1) == key) {
                        out += value;
                        text.remove_prefix(close + 1);
                        done = true;
                        break;
                    }
                }
            }
            if (!done) {
                out += '[';
                text.remove_prefix(1);
            }
        }
        return out;
    }

    constexpr std::string_view own_tag = "\x01OWN";
    constexpr std::string_view no_default = "\x01NODEFAULT";

    // The text for what happened (key) as the first of the effects (namespaces) that has one says it, or else the
    // default one. As in Showdown's client, own_tag picks the default's own variant ("Go! [FULLNAME]!") and
    // no_default stops the search with nothing. Ends with a '\n' when there is a text.
    std::string text_for(std::string_view key, std::initializer_list<std::string_view> namespaces) {
        for (const std::string_view space : namespaces) {
            if (space.empty()) {
                continue;
            }
            if (space == own_tag) {
                const auto text = lookup("default", std::string(key) + "Own");
                return text.empty() ? std::string() : std::string(text) + '\n';
            }
            if (space == no_default) {
                return {};
            }
            std::string id = effect_id(space);
            std::string type(key);
            std::string_view text = lookup(id, type);
            if (text.empty()) {
                continue;
            }
            // A few hops at most: "#crash" is Crash's text, "#.start" this effect's start text.
            for (int hop = 0; hop < 4 && starts_with(text, "#"); ++hop) {
                if (starts_with(text, "#.")) {
                    type = std::string(text.substr(2));
                } else {
                    id = std::string(text.substr(1));
                }
                text = lookup(id, type);
            }
            return text.empty() || starts_with(text, "#") ? std::string() : std::string(text) + '\n';
        }
        const auto text = lookup("default", key);
        return text.empty() ? std::string() : std::string(text) + '\n';
    }

    std::string stat_name(std::string_view stat) {
        const auto name = lookup(to_id(stat), "statName");
        return name.empty() ? std::string(stat) : std::string(name);
    }

    struct Health {
        int hp = 0;
        int maxhp = 0;
        std::string status;
    };

    // "263/263", "71/100 par" or "0 fnt".
    std::optional<Health> parse_condition(std::string_view s) {
        s = trim(s);
        if (s.empty()) {
            return std::nullopt;
        }
        Health c;
        const auto space = s.find(' ');
        const std::string_view numbers = s.substr(0, space);
        if (space != std::string_view::npos) {
            c.status = std::string(trim(s.substr(space + 1)));
        }
        const auto slash = numbers.find('/');
        c.hp = to_int(numbers.substr(0, slash), -1);
        c.maxhp = slash == std::string_view::npos ? 0 : to_int(numbers.substr(slash + 1), -1);
        if (c.hp < 0 || c.maxhp < 0 || c.hp > 100000 || c.maxhp > 100000) {
            return std::nullopt;
        }
        if (c.hp == 0) {
            c.status = "fnt";
        }
        return c;
    }

    // How much of its health a Pokémon lost or got back, as Showdown's client writes it: "34%", or "12.5%" with
    // exact HP.
    std::string percentage(double before, double after, bool exact) {
        const double change = std::abs(before - after) * 100;
        if (!exact || std::abs(change - std::round(change)) < 1e-6) {
            return std::format("{}%", static_cast<int>(std::round(change)));
        }
        return std::format("{:.1f}%", change);
    }

    // Plain text for the log: no control characters (which a terminal could take as escapes).
    std::string plain(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (const char c : s) {
            if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
                out += c;
            }
        }
        return out;
    }

    // The commands whose last arguments are text, not [key] value tags.
    bool is_text_command(std::string_view cmd) {
        for (const std::string_view c :
             {"player",      "win",   "tie",      "error",       "c",    "chat", "c:",      "-message",
              "message",     "-hint", "inactive", "inactiveoff", "raw",  "html", "request", "bigerror",
              "j",           "l",     "n",        "J",           "L",    "N",    "rule",    "tier",
              "zchat-types", "uhtml", "name",     "join",        "leave"}) {
            if (cmd == c) {
                return true;
            }
        }
        return false;
    }

    // The volatile shown for an effect that has started on a Pokémon: "Leech Seed", "Confusion", "Perish 2".
    std::string volatile_name(std::string_view effect) {
        const std::string id = effect_id(effect);
        for (const std::string_view counted : {"perish", "stockpile"}) {
            if (starts_with(id, counted) && id.size() > counted.size()) {
                return std::format("{} {}", counted == "perish" ? "Perish" : "Stockpile", id.substr(counted.size()));
            }
        }
        if (starts_with(id, "protosynthesis")) {
            return "Protosynthesis";
        }
        if (starts_with(id, "quarkdrive")) {
            return "Quark Drive";
        }
        if (const auto name = lookup(id, "name"); !name.empty()) {
            return std::string(name);
        }
        std::string name(effect_name(effect));
        if (!name.empty() && name[0] >= 'a' && name[0] <= 'z') {
            name[0] = static_cast<char>(name[0] - 'a' + 'A');
        }
        return name;
    }

    bool same_volatile(std::string_view shown, std::string_view effect) {
        const std::string a = to_id(shown);
        const std::string b = to_id(volatile_name(effect));
        // Perish Song counts down as perish3, perish2...: any of them is the same volatile.
        for (const std::string_view counted : {"perish", "stockpile"}) {
            if (starts_with(a, counted) && starts_with(b, counted)) {
                return true;
            }
        }
        return a == b;
    }

    // The members of a JSON object, written one after the other.
    class Writer {
    public:
        explicit Writer(std::string& out) :
            out_(out) {
        }
        Writer& key(std::string_view name) {
            comma();
            out_ += json::quote(name);
            out_ += ':';
            return *this;
        }
        void string(std::string_view value) {
            out_ += json::quote(value);
        }
        void number(int value) {
            out_ += std::to_string(value);
        }
        void boolean(bool value) {
            out_ += value ? "true" : "false";
        }
        void raw(std::string_view text) {
            out_ += text;
        }
        // Starts a value in an array (after its first).
        void comma() {
            if (!first_) {
                out_ += ',';
            }
            first_ = false;
        }

    private:
        std::string& out_;
        bool first_ = true;
    };

    std::string string_array(const std::vector<std::string>& values) {
        std::string out = "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                out += ',';
            }
            out += json::quote(values[i]);
        }
        return out + "]";
    }

    std::string int_object(const std::map<std::string, int>& values) {
        std::string out = "{";
        for (const auto& [key, value] : values) {
            if (out.size() > 1) {
                out += ',';
            }
            out += std::format("{}:{}", json::quote(key), value);
        }
        return out + "}";
    }

    std::string lowercase(std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

} // namespace

struct Battle::Line {
    std::string_view cmd;
    // As in Showdown's client, args[0] is the command.
    std::vector<std::string_view> args;
    // The [key] value tags; the value of a tag without one is ".".
    std::vector<std::pair<std::string_view, std::string_view>> tags;
    // Everything after the command, for the text ones.
    std::string_view rest;

    std::string_view arg(std::size_t i) const {
        return i < args.size() ? args[i] : std::string_view();
    }
    std::string_view tag(std::string_view key) const {
        for (const auto& [k, v] : tags) {
            if (k == key) {
                return v;
            }
        }
        return {};
    }
    bool has(std::string_view key) const {
        return !tag(key).empty();
    }
};

Battle::Battle(std::string side) :
    side_(side == "p1" || side == "p2" ? std::move(side) : std::string()) {
}

void Battle::feed(std::string_view message) {
    while (true) {
        const auto end = message.find('\n');
        std::string_view text = message.substr(0, end);
        if (!text.empty() && text.back() == '\r') {
            text.remove_suffix(1);
        }
        if (split_ == 2) {
            // The secret line of a |split|, exact for the side it named.
            split_ = 1;
            if (split_ours_) {
                line(text);
            }
        } else if (split_ == 1) {
            split_ = 0;
            if (!split_ours_) {
                line(text);
            }
        } else {
            line(text);
        }
        if (end == std::string_view::npos) {
            break;
        }
        message.remove_prefix(end + 1);
    }
}

void Battle::chat(std::string_view name, std::string_view color, std::string_view text) {
    log_.push_back({LogLine::Kind::Chat, plain(text), plain(name), plain(color)});
}

void Battle::notice(std::string_view text) {
    log_.push_back({LogLine::Kind::Notice, plain(text), {}, {}});
}

std::string Battle::player_name(std::string_view side) const {
    if (side == "p1" || side == "p2") {
        return sides_[side == "p1" ? 0 : 1].name;
    }
    return {};
}

int Battle::rqid() const {
    return request_ && !answered_ && !over_ ? request_->rqid : 0;
}

void Battle::chosen() {
    answered_ = true;
    error_.clear();
}

void Battle::line(std::string_view text) {
    if (text.size() < 2 || text[0] != '|') {
        return;
    }
    Line l;
    text.remove_prefix(1);
    const auto bar = text.find('|');
    l.cmd = text.substr(0, bar);
    l.rest = bar == std::string_view::npos ? std::string_view() : text.substr(bar + 1);
    if (l.cmd == "split") {
        split_ = 2;
        split_ours_ = !side_.empty() && trim(l.rest) == side_;
        return;
    }
    l.args.push_back(l.cmd);
    if (bar != std::string_view::npos) {
        std::string_view rest = l.rest;
        while (true) {
            const auto next = rest.find('|');
            l.args.push_back(rest.substr(0, next));
            if (next == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(next + 1);
        }
    }
    if (!is_text_command(l.cmd)) {
        // The tags are the last arguments that look like "[key] value", as Showdown's client takes them.
        while (l.args.size() > 1) {
            const std::string_view last = l.args.back();
            const auto close = last.find(']');
            if (last.empty() || last[0] != '[' || close == std::string_view::npos || close < 2) {
                break;
            }
            const std::string_view value = trim(last.substr(close + 1));
            l.tags.emplace_back(last.substr(1, close - 1), value.empty() ? std::string_view(".") : value);
            l.args.pop_back();
        }
    }
    say(l, describe(l));
    apply(l);
}

int Battle::side_index(std::string_view ident) {
    if (ident.size() < 2 || ident[0] != 'p' || (ident[1] != '1' && ident[1] != '2')) {
        return -1;
    }
    return ident[1] - '1';
}

namespace {

    // "p1a: Volbeat" is "p1: Volbeat", the same Pokémon wherever it is.
    std::string base_ident(std::string_view ident) {
        const auto colon = ident.find(':');
        if (colon == std::string_view::npos || colon < 2) {
            return {};
        }
        return std::format("{}: {}", ident.substr(0, 2), trim(ident.substr(colon + 1)));
    }

    std::string_view nickname(std::string_view ident) {
        const auto colon = ident.find(':');
        return colon == std::string_view::npos ? trim(ident) : trim(ident.substr(colon + 1));
    }

} // namespace

const Battle::Mon* Battle::find(std::string_view ident) const {
    const int side = side_index(ident);
    const std::string base = base_ident(ident);
    if (side < 0 || base.empty()) {
        return nullptr;
    }
    // A Pokémon in battle is the active one, even if another of the team has its name (which Illusion can do).
    const Side& s = sides_[side];
    if (ident.size() > 2 && ident[2] != ':' && s.active >= 0 && s.active < static_cast<int>(s.team.size()) &&
        s.team[s.active].ident == base) {
        return &s.team[s.active];
    }
    for (const Mon& mon : s.team) {
        if (mon.ident == base) {
            return &mon;
        }
    }
    return nullptr;
}

Battle::Mon* Battle::find(std::string_view ident) {
    return const_cast<Mon*>(std::as_const(*this).find(ident));
}

Battle::Mon* Battle::active(int side) {
    Side& s = sides_[side];
    return s.active >= 0 && s.active < static_cast<int>(s.team.size()) ? &s.team[s.active] : nullptr;
}

bool Battle::near(int side) const {
    // A spectator sees the battle from p1's side, as on Showdown.
    return side == (side_ == "p2" ? 1 : 0);
}

bool Battle::own(int side) const {
    return !side_.empty() && side == (side_ == "p2" ? 1 : 0);
}

std::string Battle::trainer(int side) const {
    if (side < 0 || side > 1) {
        return {};
    }
    return sides_[side].name.empty() ? std::format("Player {}", side + 1) : sides_[side].name;
}

std::string Battle::pokemon_text(std::string_view ident) const {
    if (ident.empty()) {
        return {};
    }
    const int side = side_index(ident);
    if (side < 0) {
        return std::string(effect_name(ident));
    }
    return fill(lookup("default", near(side) ? "pokemon" : "opposingPokemon"),
                {{"[NICKNAME]", nickname(ident)}, {"[TRAINER]", trainer(side)}});
}

std::string Battle::team_text(int side) const {
    return std::string(lookup("default", near(side) ? "team" : "opposingTeam"));
}

std::string Battle::describe(const Line& l) const {
    const std::string_view cmd = l.cmd;
    if (l.has("silent")) {
        return {};
    }
    const std::string_view from = l.tag("from");
    const std::string_view of = l.tag("of");
    const auto P = [this](std::string_view ident) {
        return pokemon_text(ident);
    };
    // "[The opposing Salamence's Intimidate]", when an ability did it.
    const auto ability = [&](std::string_view name, std::string_view holder) -> std::string {
        if (name.empty()) {
            return {};
        }
        return fill(lookup("default", "abilityActivation"),
                    {{"[POKEMON]", P(holder)}, {"[ABILITY]", effect_name(name)}}) +
               '\n';
    };
    const auto maybe_ability = [&](std::string_view effect, std::string_view holder) -> std::string {
        if (!starts_with(effect, "ability:")) {
            return {};
        }
        return ability(effect.substr(8), holder);
    };
    const std::string_view a1 = l.arg(1);
    const std::string_view a2 = l.arg(2);
    const std::string_view a3 = l.arg(3);
    const std::string_view of_or_1 = of.empty() ? a1 : of;

    if (cmd == "start") {
        std::string text = text_for("startBattle", {});
        // Showdown fills the two [TRAINER]s in turn.
        const auto at = text.find("[TRAINER]");
        if (at != std::string::npos) {
            text.replace(at, 9, trainer(0));
            const auto again = text.find("[TRAINER]", at + trainer(0).size());
            if (again != std::string::npos) {
                text.replace(again, 9, trainer(1));
            }
        }
        return text;
    }
    if (cmd == "turn") {
        return std::format("Turn {}", to_int(a1));
    }
    if (cmd == "win" || cmd == "tie") {
        if (cmd == "tie" || trim(l.rest).empty()) {
            return std::format("Tie between {} and {}!", trainer(0), trainer(1));
        }
        return fill(text_for("winBattle", {}), {{"[TRAINER]", trim(l.rest)}});
    }
    if (cmd == "error") {
        return std::string(l.rest);
    }
    if (cmd == "-message" || cmd == "message" || cmd == "bigerror" || cmd == "inactive" || cmd == "inactiveoff") {
        return std::string(l.rest);
    }
    if (cmd == "-hint") {
        return std::format("  ({})", l.rest);
    }
    if (cmd == "switch" || cmd == "drag" || cmd == "replace") {
        const int side = side_index(a1);
        if (side < 0) {
            return {};
        }
        std::string out;
        const Side& s = sides_[side];
        const Mon* previous = s.active >= 0 && s.active < static_cast<int>(s.team.size()) ? &s.team[s.active] : nullptr;
        if (cmd == "switch" && previous && previous->status != "fnt" && previous->ident != base_ident(a1)) {
            // Moves that switch the user out have words of their own: "Gholdengo went back to Ash!".
            const std::string_view by = s.last_move == "uturn" || s.last_move == "voltswitch" ||
                                                s.last_move == "flipturn" || s.last_move == "partingshot" ||
                                                s.last_move == "batonpass" || s.last_move == "shedtail" ||
                                                s.last_move == "teleport" || s.last_move == "chillyreception"
                                            ? std::string_view(s.last_move)
                                            : std::string_view();
            const std::string ident = std::format("p{}a: {}", side + 1, previous->name);
            out += fill(text_for("switchOut", {by, own(side) ? own_tag : std::string_view()}),
                        {{"[TRAINER]", trainer(side)}, {"[NICKNAME]", previous->name}, {"[POKEMON]", P(ident)}});
        }
        if (cmd == "replace") {
            return out;
        }
        const std::string_view species = trim(a2.substr(0, a2.find(',')));
        const std::string_view name = nickname(a1);
        const std::string full =
            name == species ? std::format("**{}**", species) : std::format("{} (**{}**)", name, species);
        const std::string_view key = cmd == "switch" ? "switchIn" : "drag";
        out += fill(text_for(key, {own(side) && cmd == "switch" ? own_tag : std::string_view()}),
                    {{"[TRAINER]", trainer(side)}, {"[FULLNAME]", full}});
        return out;
    }
    if (cmd == "faint") {
        return fill(text_for("faint", {}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "move") {
        return fill(text_for("move", {from}), {{"[POKEMON]", P(a1)}, {"[MOVE]", a2}});
    }
    if (cmd == "cant") {
        std::string text = text_for("cant", {a2, no_default});
        if (text.empty()) {
            text = text_for(a3.empty() ? "cantNoMove" : "cant", {});
        }
        return maybe_ability(a2, of_or_1) + fill(text, {{"[POKEMON]", P(a1)}, {"[MOVE]", a3}});
    }
    if (cmd == "detailschange" || cmd == "-formechange") {
        const std::string species =
            cmd == "detailschange" ? std::string(trim(a2.substr(0, a2.find(',')))) : std::string(trim(a2));
        const std::string id = to_id(species);
        std::string_view effect;
        std::string_view key = "transform";
        // The forme changes that have words of their own, and the effect whose words they are; back[] are the ones
        // back to the base forme.
        static constexpr std::pair<std::string_view, std::string_view> formes[] = {
            {"greninjaash", "battlebond"},         {"mimikyubusted", "disguise"},
            {"zygardecomplete", "powerconstruct"}, {"necrozmaultra", "ultranecroziumz"},
            {"darmanitanzen", "zenmode"},          {"darmanitangalarzen", "zenmode"},
            {"aegislashblade", "stancechange"},    {"wishiwashischool", "schooling"},
            {"miniormeteor", "shieldsdown"},       {"eiscuenoice", "iceface"},
            {"terapagosterastal", "terashift"}};
        static constexpr std::pair<std::string_view, std::string_view> back[] = {
            {"darmanitan", "zenmode"},   {"darmanitangalar", "zenmode"}, {"aegislash", "stancechange"},
            {"wishiwashi", "schooling"}, {"minior", "shieldsdown"},      {"eiscue", "iceface"}};
        for (const auto& [forme, by] : formes) {
            if (id == forme) {
                effect = by;
            }
        }
        for (const auto& [forme, by] : back) {
            if (id == forme) {
                effect = by;
                key = "transformEnd";
            }
        }
        const std::string text = text_for(key, {effect, l.has("msg") ? std::string_view() : no_default});
        return maybe_ability(from, a1) + fill(text, {{"[POKEMON]", P(a1)}, {"[SPECIES]", species}});
    }
    if (cmd == "-transform") {
        return maybe_ability(from, a1) + fill(text_for("transform", {"Transform"}),
                                              {{"[POKEMON]", P(a1)},
                                               {"[TARGET]", P(a2)},
                                               {"[SPECIES]", side_index(a2) >= 0 ? P(a2) : std::string(a2)}});
    }
    if (cmd == "-damage") {
        const std::string line1 = maybe_ability(from, of_or_1);
        if (std::string text = text_for("damage", {from, no_default}); !text.empty()) {
            return line1 + fill(text, {{"[POKEMON]", P(a1)}});
        }
        if (from.empty()) {
            std::string lost;
            const Mon* mon = find(a1);
            const auto after = parse_condition(a2);
            if (mon && after && mon->maxhp > 0 && (after->maxhp > 0 || after->hp == 0)) {
                const double before = static_cast<double>(mon->hp) / mon->maxhp;
                const double now = after->maxhp > 0 ? static_cast<double>(after->hp) / after->maxhp : 0.0;
                lost = percentage(before, now, (after->maxhp > 0 ? after->maxhp : mon->maxhp) != 100);
            }
            const std::string text = text_for(lost.empty() ? "damage" : "damagePercentage", {});
            return line1 + fill(text, {{"[POKEMON]", P(a1)}, {"[PERCENTAGE]", lost}});
        }
        if (starts_with(from, "item:")) {
            return line1 + fill(text_for(of.empty() ? "damageFromItem" : "damageFromPokemon", {}),
                                {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(from)}, {"[SOURCE]", P(of)}});
        }
        if (l.has("partiallytrapped") || effect_id(from) == "bind" || effect_id(from) == "wrap") {
            return line1 + fill(text_for("damageFromPartialTrapping", {}),
                                {{"[POKEMON]", P(a1)}, {"[MOVE]", effect_name(from)}});
        }
        return line1 + fill(text_for("damage", {}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-heal") {
        const std::string line1 = maybe_ability(from, a1);
        if (std::string text = text_for("heal", {from, no_default}); !text.empty()) {
            return line1 + fill(text, {{"[POKEMON]", P(a1)}, {"[SOURCE]", P(of)}, {"[NICKNAME]", l.tag("wisher")}});
        }
        if (!from.empty() && !starts_with(from, "ability:")) {
            return line1 +
                   fill(text_for("healFromEffect", {}), {{"[POKEMON]", P(a1)}, {"[EFFECT]", effect_name(from)}});
        }
        return line1 + fill(text_for("heal", {}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-sethp") {
        return fill(text_for("activate", {from, no_default}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-boost" || cmd == "-unboost") {
        const int amount = to_int(a3);
        const std::string line1 = maybe_ability(from, of_or_1);
        std::string key(cmd.substr(1));
        if (amount >= 3) {
            key += '3';
        } else if (amount >= 2) {
            key += '2';
        } else if (amount == 0) {
            key += '0';
        }
        if (amount != 0 && starts_with(from, "item:")) {
            return line1 + fill(text_for(key + "FromItem", {from}),
                                {{"[POKEMON]", P(a1)}, {"[STAT]", stat_name(a2)}, {"[ITEM]", effect_name(from)}});
        }
        return line1 + fill(text_for(key, {from}), {{"[POKEMON]", P(a1)}, {"[STAT]", stat_name(a2)}});
    }
    if (cmd == "-setboost") {
        return maybe_ability(from, of_or_1) +
               fill(text_for("boost", {from}), {{"[POKEMON]", P(a1)}, {"[STAT]", stat_name(a2)}});
    }
    if (cmd == "-swapboost") {
        return maybe_ability(from, a1) +
               fill(text_for("swapBoost", {from}), {{"[POKEMON]", P(a1)}, {"[TARGET]", P(a2)}});
    }
    if (cmd == "-copyboost") {
        return maybe_ability(from, a1) +
               fill(text_for("copyBoost", {from}), {{"[POKEMON]", P(a1)}, {"[TARGET]", P(a2)}});
    }
    if (cmd == "-clearboost" || cmd == "-clearpositiveboost" || cmd == "-clearnegativeboost") {
        return maybe_ability(from, of_or_1) +
               fill(text_for(l.has("zeffect") ? "clearBoostFromZEffect" : "clearBoost", {from}),
                    {{"[POKEMON]", P(a1)}, {"[SOURCE]", P(a2)}});
    }
    if (cmd == "-invertboost") {
        return maybe_ability(from, a1) + fill(text_for("invertBoost", {from}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-clearallboost") {
        return text_for("clearAllBoost", {from});
    }
    if (cmd == "-crit" || cmd == "-supereffective" || cmd == "-resisted") {
        std::string key = cmd == "-crit" ? "crit" : cmd == "-supereffective" ? "superEffective" : "resisted";
        if (l.has("spread")) {
            key += "Spread";
        }
        return fill(text_for(key, {}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-immune") {
        std::string text = text_for("block", {from});
        if (text.empty()) {
            text = text_for(a1.empty() ? "immuneNoPokemon" : l.has("ohko") ? "immuneOHKO" : "immune", {from});
        }
        return maybe_ability(from, of_or_1) + fill(text, {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-miss") {
        const std::string line1 = maybe_ability(from, of.empty() ? a2 : of);
        if (a2.empty()) {
            return line1 + fill(text_for("missNoPokemon", {}), {{"[SOURCE]", P(a1)}});
        }
        return line1 + fill(text_for("miss", {}), {{"[POKEMON]", P(a2)}});
    }
    if (cmd == "-fail") {
        const std::string id = effect_id(a2);
        const std::string blocker = effect_id(from);
        const std::string line1 = maybe_ability(from, of_or_1);
        std::string_view key = "block";
        if ((blocker == "desolateland" || blocker == "primordialsea") && id != "sunnyday" && id != "raindance" &&
            id != "sandstorm" && id != "hail" && id != "snowscape" && id != "chillyreception") {
            key = "blockMove";
        } else if (blocker == "uproar" && l.has("msg")) {
            key = "blockSelf";
        }
        if (std::string text = text_for(key, {from, no_default}); !text.empty()) {
            return line1 + fill(text, {{"[POKEMON]", P(a1)}});
        }
        if (id == "unboost") {
            return line1 + fill(text_for(a3.empty() ? "fail" : "failSingular", {"unboost"}),
                                {{"[POKEMON]", P(a1)}, {"[STAT]", stat_name(a3)}});
        }
        key = "fail";
        if (id == "brn" || id == "frz" || id == "par" || id == "psn" || id == "tox" || id == "slp" ||
            id == "substitute" || id == "shedtail") {
            key = "alreadyStarted";
        }
        if (l.has("heavy")) {
            key = "failTooHeavy";
        }
        if (l.has("weak")) {
            key = "fail";
        }
        if (l.has("forme")) {
            key = "failWrongForme";
        }
        std::string text = text_for(key, {id});
        if (text.empty()) {
            text = text_for("fail", {});
        }
        return line1 + fill(text, {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-block") {
        return maybe_ability(a2, of_or_1) +
               fill(text_for("block", {a2}),
                    {{"[POKEMON]", P(a1)}, {"[SOURCE]", P(l.arg(4).empty() ? of : l.arg(4))}, {"[MOVE]", a3}});
    }
    if (cmd == "-notarget") {
        return text_for("noTarget", {});
    }
    if (cmd == "-ohko" || cmd == "-center" || cmd == "-combine") {
        return text_for(cmd == "-ohko" ? "ohko" : cmd == "-center" ? "center" : "combine", {});
    }
    if (cmd == "-hitcount") {
        if (trim(a2) == "1") {
            return text_for("hitCountSingular", {});
        }
        return fill(text_for("hitCount", {}), {{"[NUMBER]", trim(a2)}});
    }
    if (cmd == "-status") {
        const std::string line1 = maybe_ability(from, of_or_1);
        std::string text;
        if (effect_id(from) == "rest") {
            text = text_for("startFromRest", {a2});
        } else if (starts_with(from, "item:")) {
            text = text_for("startFromItem", {a2, no_default});
        }
        if (text.empty()) {
            text = text_for("start", {a2});
        }
        return line1 + fill(text, {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(from)}});
    }
    if (cmd == "-curestatus") {
        if (effect_id(from) == "naturalcure") {
            return fill(text_for("activate", {from}), {{"[POKEMON]", P(a1)}});
        }
        const std::string line1 = maybe_ability(from, of_or_1);
        if (starts_with(from, "item:")) {
            return line1 + fill(text_for("endFromItem", {a2}), {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(from)}});
        }
        if (l.has("thaw")) {
            return line1 + fill(text_for("endFromMove", {a2}), {{"[POKEMON]", P(a1)}, {"[MOVE]", effect_name(from)}});
        }
        std::string text = text_for("end", {a2, no_default});
        if (text.empty()) {
            text = fill(text_for("end", {}), {{"[EFFECT]", "status"}});
        }
        return line1 + fill(text, {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-cureteam") {
        return text_for("activate", {from});
    }
    if (cmd == "-singleturn" || cmd == "-singlemove") {
        std::string line1 = maybe_ability(a2, of_or_1);
        if (line1.empty()) {
            line1 = maybe_ability(from, of_or_1);
        }
        if (effect_id(a2) == "instruct") {
            return line1 + fill(text_for("activate", {a2}), {{"[POKEMON]", P(of)}, {"[TARGET]", P(a1)}});
        }
        std::string text = text_for("start", {a2, no_default});
        if (text.empty()) {
            text = fill(text_for("start", {}), {{"[EFFECT]", effect_name(a2)}});
        }
        return line1 + fill(text, {{"[POKEMON]", P(a1)}, {"[SOURCE]", P(of)}, {"[TEAM]", team_text(side_index(a1))}});
    }
    if (cmd == "-start") {
        std::string line1 = maybe_ability(a2, of_or_1);
        if (line1.empty()) {
            line1 = maybe_ability(from, of_or_1);
        }
        const std::string id = effect_id(a2);
        if (id == "typechange") {
            return line1 +
                   fill(text_for("typeChange", {from}), {{"[POKEMON]", P(a1)}, {"[TYPE]", a3}, {"[SOURCE]", P(of)}});
        }
        if (id == "typeadd") {
            return line1 + fill(text_for("typeAdd", {from}), {{"[POKEMON]", P(a1)}, {"[TYPE]", a3}});
        }
        if (starts_with(id, "stockpile") && id.size() > 9) {
            return line1 + fill(text_for("start", {"stockpile"}), {{"[POKEMON]", P(a1)}, {"[NUMBER]", id.substr(9)}});
        }
        if (starts_with(id, "perish") && id.size() > 6) {
            return line1 +
                   fill(text_for("activate", {"perishsong"}), {{"[POKEMON]", P(a1)}, {"[NUMBER]", id.substr(6)}});
        }
        if ((starts_with(id, "protosynthesis") || starts_with(id, "quarkdrive")) && id.size() > 3) {
            const std::string_view effect = starts_with(id, "quarkdrive") ? "quarkdrive" : "protosynthesis";
            return line1 + fill(text_for("start", {effect}),
                                {{"[POKEMON]", P(a1)}, {"[STAT]", stat_name(id.substr(id.size() - 3))}});
        }
        std::string_view key = "start";
        if (l.has("already")) {
            key = "alreadyStarted";
        }
        if (l.has("fatigue")) {
            key = "startFromFatigue";
        }
        if (l.has("zeffect")) {
            key = "startFromZEffect";
        }
        if (l.has("damage")) {
            key = "activate";
        }
        if (l.has("block")) {
            key = "block";
        }
        if (l.has("upkeep")) {
            key = "upkeep";
        }
        std::string text;
        if (key == "start" && starts_with(from, "item:")) {
            text = text_for("startFromItem", {from, a2, no_default});
        }
        if (text.empty()) {
            text = text_for(key, {from, a2});
        }
        return line1 + fill(text, {{"[POKEMON]", P(a1)},
                                   {"[EFFECT]", effect_name(a2)},
                                   {"[MOVE]", a3},
                                   {"[SOURCE]", P(of)},
                                   {"[ITEM]", effect_name(from)}});
    }
    if (cmd == "-end") {
        std::string line1 = maybe_ability(a2, of_or_1);
        if (line1.empty()) {
            line1 = maybe_ability(from, of_or_1);
        }
        const std::string id = effect_id(a2);
        if (id == "doomdesire" || id == "futuresight") {
            return line1 + fill(text_for("activate", {a2}), {{"[TARGET]", P(a1)}});
        }
        std::string text;
        if (starts_with(from, "item:")) {
            text = text_for("endFromItem", {a2, no_default});
        }
        if (text.empty()) {
            // Effects that count down are known by their name: perish3 ends as Perish Song.
            text =
                text_for("end", {starts_with(id, "stockpile") ? std::string_view("stockpile") : std::string_view(a2)});
        }
        return line1 + fill(text, {{"[POKEMON]", P(a1)},
                                   {"[EFFECT]", effect_name(a2)},
                                   {"[SOURCE]", P(of)},
                                   {"[ITEM]", effect_name(from)}});
    }
    if (cmd == "-item") {
        const std::string id = effect_id(from);
        std::string_view source = of;
        std::string_view holder = of;
        std::string_view target;
        if (id == "magician" || id == "pickpocket") {
            target = of;
            holder = {};
        }
        const std::string line1 = maybe_ability(from, holder.empty() ? a1 : holder);
        if (id == "thief" || id == "covet" || id == "bestow" || id == "magician" || id == "pickpocket") {
            return line1 + fill(text_for("takeItem", {from}), {{"[POKEMON]", P(a1)},
                                                               {"[ITEM]", effect_name(a2)},
                                                               {"[SOURCE]", P(target.empty() ? source : target)}});
        }
        if (id == "frisk") {
            const bool target_seen = !of.empty() && !a1.empty() && of != a1;
            return line1 + fill(text_for(target_seen ? "activate" : "activateNoTarget", {"Frisk"}),
                                {{"[POKEMON]", P(of)}, {"[ITEM]", effect_name(a2)}, {"[TARGET]", P(a1)}});
        }
        if (!from.empty()) {
            return line1 + fill(text_for("addItem", {from}), {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(a2)}});
        }
        return line1 + fill(text_for("start", {a2, no_default}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-enditem") {
        const std::string line1 = maybe_ability(from, of_or_1);
        if (l.has("eat")) {
            return line1 + fill(text_for("eatItem", {from}), {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(a2)}});
        }
        const std::string id = effect_id(from);
        if (id == "gem") {
            return line1 + fill(text_for("useGem", {a2}),
                                {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(a2)}, {"[MOVE]", l.tag("move")}});
        }
        if (id == "stealeat") {
            return line1 +
                   fill(text_for("removeItem", {"Bug Bite"}), {{"[SOURCE]", P(of)}, {"[ITEM]", effect_name(a2)}});
        }
        if (!from.empty()) {
            return line1 + fill(text_for("removeItem", {from}),
                                {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(a2)}, {"[SOURCE]", P(of)}});
        }
        if (l.has("weaken")) {
            return line1 + fill(text_for("activateWeaken", {}), {{"[POKEMON]", P(a1)}, {"[ITEM]", effect_name(a2)}});
        }
        std::string text = text_for("end", {a2, no_default});
        if (text.empty()) {
            text = fill(text_for("activateItem", {}), {{"[ITEM]", effect_name(a2)}});
        }
        return line1 + fill(text, {{"[POKEMON]", P(a1)}, {"[TARGET]", P(of)}});
    }
    if (cmd == "-ability") {
        std::string_view old_ability = a3;
        std::string_view arg4 = l.arg(4);
        if (side_index(old_ability) >= 0 || old_ability == "boost") {
            arg4 = old_ability;
            old_ability = {};
        }
        std::string line1 = ability(old_ability, a1) + ability(a2, a1);
        if (l.has("fail")) {
            return line1 + text_for("block", {from});
        }
        if (!from.empty()) {
            line1 = maybe_ability(from, a1) + line1;
            return line1 + fill(text_for("changeAbility", {from}),
                                {{"[POKEMON]", P(a1)}, {"[ABILITY]", effect_name(a2)}, {"[SOURCE]", P(of)}});
        }
        const std::string id = effect_id(a2);
        if (id == "unnerve") {
            const int side = side_index(a1);
            return line1 + fill(text_for("start", {a2}), {{"[TEAM]", team_text(side < 0 ? 0 : 1 - side)}});
        }
        const std::string_view key = id == "anticipation" || id == "sturdy" ? "activate" : "start";
        return line1 + fill(text_for(key, {a2, no_default}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-endability") {
        if (!a2.empty()) {
            return ability(a2, a1);
        }
        return maybe_ability(from, a1) + fill(text_for("start", {"Gastro Acid"}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-terastallize") {
        return fill(text_for("terastallize", {}), {{"[POKEMON]", P(a1)}, {"[TYPE]", a2}});
    }
    if (cmd == "-mega") {
        return fill(text_for("mega", {a2}),
                    {{"[POKEMON]", P(a1)}, {"[ITEM]", a3}, {"[TRAINER]", trainer(side_index(a1))}});
    }
    if (cmd == "-primal") {
        return fill(text_for("primal", {}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-burst") {
        return fill(text_for("activate", {"Ultranecrozium Z"}), {{"[POKEMON]", P(a1)}});
    }
    if (cmd == "-activate") {
        std::string_view pokemon = a1;
        std::string_view target = a3;
        const std::string id = effect_id(a2);
        if (id == "celebrate") {
            return fill(text_for("activate", {"celebrate"}), {{"[TRAINER]", trainer(side_index(a1))}});
        }
        if (target.empty() && (id == "hyperdrill" || id == "hyperspacefury" || id == "hyperspacehole" ||
                               id == "phantomforce" || id == "shadowforce" || id == "feint")) {
            target = pokemon;
            pokemon = of.empty() ? target : of;
        }
        if (target.empty()) {
            target = of.empty() ? pokemon : of;
        }
        std::string line1 = maybe_ability(a2, pokemon);
        if (id == "lockon" || id == "mindreader") {
            return line1 + fill(text_for("start", {a2}), {{"[POKEMON]", P(of)}, {"[SOURCE]", P(pokemon)}});
        }
        std::string_view key = "activate";
        if (id == "forewarn" && pokemon == target) {
            key = "activateNoTarget";
        }
        if ((id == "protosynthesis" || id == "quarkdrive") && l.has("fromitem")) {
            key = "activateFromItem";
        }
        if (id == "orichalcumpulse" && l.has("source")) {
            key = "start";
        }
        std::string text = text_for(key, {a2, no_default});
        if (text.empty()) {
            // Protect and the like say again what they do when they block a move.
            text = text_for("block", {a2, no_default});
        }
        if (text.empty()) {
            if (!line1.empty()) {
                return line1;
            }
            return fill(text_for("activate", {}), {{"[EFFECT]", effect_name(a2)}});
        }
        if (id == "brickbreak") {
            text = fill(text, {{"[TEAM]", team_text(side_index(target))}});
        }
        line1 += ability(l.tag("ability"), pokemon) + ability(l.tag("ability2"), target);
        // The move of a Leppa Berry comes as its third argument.
        const std::string_view move = l.tag("move").empty() && side_index(a3) < 0 ? a3 : l.tag("move");
        return line1 + fill(text, {{"[POKEMON]", P(pokemon)},
                                   {"[TARGET]", P(target)},
                                   {"[SOURCE]", P(of)},
                                   {"[MOVE]", move},
                                   {"[NUMBER]", l.tag("number")},
                                   {"[ITEM]", l.tag("item")},
                                   {"[NAME]", l.tag("name")}});
    }
    if (cmd == "-prepare") {
        return fill(text_for("prepare", {a2}), {{"[POKEMON]", P(a1)}, {"[TARGET]", P(a3)}});
    }
    if (cmd == "-waiting") {
        return fill(text_for("activate", {"Water Pledge"}), {{"[POKEMON]", P(a1)}, {"[TARGET]", P(a2)}});
    }
    if (cmd == "-weather") {
        if (a1.empty() || a1 == "none") {
            return text_for("end", {weather_id_, no_default});
        }
        if (l.has("upkeep")) {
            return text_for("upkeep", {a1, no_default});
        }
        return maybe_ability(from, of) + text_for("start", {a1, no_default});
    }
    if (cmd == "-fieldstart" || cmd == "-fieldactivate") {
        const std::string line1 = maybe_ability(from, of);
        if (effect_id(from) == "hadronengine") {
            return line1 + fill(text_for("start", {"hadronengine"}), {{"[POKEMON]", P(of)}});
        }
        const std::string_view key = cmd == "-fieldstart" || effect_id(a1) == "perishsong" ? "start" : "activate";
        std::string text = text_for(key, {a1, no_default});
        if (text.empty()) {
            text = fill(text_for("startFieldEffect", {}), {{"[EFFECT]", effect_name(a1)}});
        }
        return line1 + fill(text, {{"[POKEMON]", P(of)}});
    }
    if (cmd == "-fieldend") {
        std::string text = text_for("end", {a1, no_default});
        if (text.empty()) {
            text = fill(text_for("endFieldEffect", {}), {{"[EFFECT]", effect_name(a1)}});
        }
        return text;
    }
    if (cmd == "-sidestart" || cmd == "-sideend") {
        const int side = side_index(a1);
        if (side < 0) {
            return {};
        }
        const bool start = cmd == "-sidestart";
        std::string text = text_for(start ? "start" : "end", {a2, no_default});
        if (text.empty()) {
            text = fill(text_for(start ? "startTeamEffect" : "endTeamEffect", {}), {{"[EFFECT]", effect_name(a2)}});
        }
        return fill(text, {{"[TEAM]", team_text(side)},
                           {"[PARTY]", lookup("default", near(side) ? "party" : "opposingParty")}});
    }
    if (cmd == "-swapsideconditions") {
        return text_for("activate", {"Court Change"});
    }
    return {};
}

void Battle::say(const Line& l, std::string_view text) {
    const std::string_view cmd = l.cmd;
    LogLine::Kind kind = LogLine::Kind::Text;
    if (cmd == "turn") {
        kind = LogLine::Kind::Turn;
    } else if (cmd == "win" || cmd == "tie") {
        kind = LogLine::Kind::Result;
    } else if (cmd == "error") {
        kind = LogLine::Kind::Error;
    } else if (cmd == "inactive" || cmd == "inactiveoff" || cmd == "bigerror") {
        kind = LogLine::Kind::Notice;
    }
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string s = plain(trim(text.substr(0, end)));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        // Showdown's markup for bold.
        for (auto at = s.find("**"); at != std::string::npos; at = s.find("**", at)) {
            s.erase(at, 2);
        }
        if (s.empty()) {
            continue;
        }
        // "the opposing Volbeat" starts a sentence as "The opposing Volbeat", after a "(" or "[" too.
        const std::size_t first = s[0] == '(' || s[0] == '[' ? 1 : 0;
        for (const std::string_view lower : {"the opposing ", "your "}) {
            if (s.compare(first, lower.size(), lower) == 0) {
                s[first] = static_cast<char>(s[first] - 'a' + 'A');
            }
        }
        LogLine::Kind line_kind = kind;
        if (kind == LogLine::Kind::Text) {
            if (cmd == "move" && s.find(" used ") != std::string::npos) {
                line_kind = LogLine::Kind::Move;
            } else if (s[0] == '(' || s[0] == '[' || cmd == "-damage" || cmd == "-heal" || cmd == "-sethp") {
                line_kind = LogLine::Kind::Minor;
            }
        }
        // Pain Split sets the HP of both Pokémon, with one text.
        if (cmd == "-sethp" && !log_.empty() && log_.back().text == s) {
            continue;
        }
        log_.push_back({line_kind, std::move(s), {}, {}});
    }
}

void Battle::set_hp(Mon& mon, std::string_view condition) const {
    const auto c = parse_condition(condition);
    if (!c) {
        return;
    }
    mon.hp = c->hp;
    if (c->maxhp > 0) {
        mon.maxhp = c->maxhp;
    }
    // Every HP comes with the status (or none).
    mon.status = c->status == "fnt" || c->status == "brn" || c->status == "par" || c->status == "slp" ||
                         c->status == "frz" || c->status == "psn" || c->status == "tox"
                     ? c->status
                     : std::string();
}

namespace {

    // "Calyrex-Ice, L72, shiny, F, tera:Steel".
    struct Details {
        std::string species;
        int level = 100;
        std::string gender;
        bool shiny = false;
        std::string tera;
    };

    Details parse_details(std::string_view details) {
        Details d;
        bool first = true;
        for (const std::string& part : split(details, ',')) {
            if (first) {
                d.species = part;
                first = false;
            } else if (part.size() > 1 && part[0] == 'L') {
                d.level = std::clamp(to_int(std::string_view(part).substr(1), 100), 1, 1000);
            } else if (part == "M" || part == "F") {
                d.gender = part;
            } else if (part == "shiny") {
                d.shiny = true;
            } else if (starts_with(part, "tera:")) {
                d.tera = part.substr(5);
            }
        }
        return d;
    }

    void clear_battle_state(auto& mon) {
        mon.boosts.clear();
        mon.volatiles.clear();
        mon.changed_types.clear();
    }

} // namespace

void Battle::reveal(std::string_view effect, std::string_view holder) {
    Mon* mon = find(holder);
    if (!mon) {
        return;
    }
    if (starts_with(effect, "item:")) {
        mon->item = std::string(effect_name(effect));
    } else if (starts_with(effect, "ability:")) {
        mon->ability = std::string(effect_name(effect));
    }
}

void Battle::switch_in(const Line& l) {
    const int side = side_index(l.arg(1));
    const std::string ident = base_ident(l.arg(1));
    if (side < 0 || ident.empty()) {
        return;
    }
    Side& s = sides_[side];
    const Details details = parse_details(l.arg(2));
    if (details.species.empty()) {
        return;
    }
    const int previous = s.active >= 0 && s.active < static_cast<int>(s.team.size()) ? s.active : -1;
    int index = -1;
    if (l.cmd == "replace" && previous >= 0) {
        // Illusion is broken: the one in battle was another Pokémon all along.
        Mon& shown = s.team[previous];
        for (std::size_t i = 0; i < s.team.size(); ++i) {
            if (s.team[i].ident == ident && static_cast<int>(i) != previous) {
                index = static_cast<int>(i);
            }
        }
        if (index < 0 && shown.fresh) {
            // It was only ever seen as the disguise: it is the Illusion user that was seen, under another name.
            shown.ident = ident;
            index = previous;
        } else {
            if (index < 0) {
                s.team.push_back({});
                index = static_cast<int>(s.team.size()) - 1;
                s.team.back().ident = ident;
            }
            Mon& real = s.team[index];
            Mon& disguise = s.team[previous];
            real.hp = disguise.hp;
            real.maxhp = disguise.maxhp;
            real.status = disguise.status;
            real.boosts = std::move(disguise.boosts);
            real.volatiles = std::move(disguise.volatiles);
            disguise.boosts.clear();
            disguise.volatiles.clear();
        }
    } else {
        for (std::size_t i = 0; i < s.team.size(); ++i) {
            if (s.team[i].ident == ident) {
                index = static_cast<int>(i);
            }
        }
        if (index < 0) {
            if (s.team.size() >= 24) {
                return;
            }
            s.team.push_back({});
            index = static_cast<int>(s.team.size()) - 1;
            s.team.back().ident = ident;
            s.team.back().fresh = true;
        } else if (index != previous) {
            s.team[index].fresh = false;
        }
        if (previous >= 0 && previous != index) {
            Mon& out = s.team[previous];
            Mon& in = s.team[index];
            // Baton Pass passes the stat changes and most volatiles on; Shed Tail its substitute.
            if (s.last_move == "batonpass") {
                in.boosts = out.boosts;
                for (const std::string& v : out.volatiles) {
                    if (to_id(v) != "taunt" && to_id(v) != "encore" && to_id(v) != "disable") {
                        in.volatiles.push_back(v);
                    }
                }
            } else if (s.last_move == "shedtail" && std::ranges::any_of(out.volatiles, [](const std::string& v) {
                           return v == "Substitute";
                       })) {
                in.volatiles.push_back("Substitute");
            }
            clear_battle_state(out);
            out.fresh = false;
        }
    }
    Mon& mon = s.team[index];
    mon.name = std::string(nickname(l.arg(1)));
    mon.species = details.species;
    mon.level = details.level;
    mon.gender = details.gender;
    mon.shiny = details.shiny;
    if (!details.tera.empty()) {
        mon.tera = details.tera;
    }
    mon.exact = own(side);
    set_hp(mon, l.arg(3));
    s.active = index;
    s.last_move.clear();
}

void Battle::apply(const Line& l) {
    const std::string_view cmd = l.cmd;
    const std::string_view a1 = l.arg(1);
    const std::string_view a2 = l.arg(2);
    const std::string_view a3 = l.arg(3);
    const std::string_view from = l.tag("from");
    const std::string_view of = l.tag("of");
    const int side = side_index(a1);
    // Whatever the line, [from] an item or ability shows that its holder has it.
    if (!from.empty()) {
        reveal(from, of.empty() ? a1 : of);
    }

    if (cmd == "player") {
        if (side >= 0 && !trim(a2).empty()) {
            sides_[side].name = std::string(trim(a2));
        }
    } else if (cmd == "teamsize") {
        if (side >= 0) {
            sides_[side].team_size = std::clamp(to_int(a2, 6), 1, 24);
        }
    } else if (cmd == "turn") {
        turn_ = std::max(0, to_int(a1, turn_));
        sides_[0].last_move.clear();
        sides_[1].last_move.clear();
    } else if (cmd == "win" || cmd == "tie") {
        over_ = true;
        winner_ = cmd == "win" ? std::string(trim(l.rest)) : std::string();
        request_.reset();
    } else if (cmd == "request") {
        request(l.rest);
    } else if (cmd == "error") {
        // The simulator refused our choice: the request is waiting for another one.
        if (request_ && !over_) {
            answered_ = false;
            error_ = plain(l.rest);
        }
    } else if (cmd == "switch" || cmd == "drag" || cmd == "replace") {
        switch_in(l);
    } else if (cmd == "poke") {
        // Team Preview: the Pokémon each side brings.
        const Details details = parse_details(a2);
        if (side >= 0 && !details.species.empty() && sides_[side].team.size() < 24 &&
            std::ranges::none_of(sides_[side].team, [&](const Mon& m) {
                return m.species == details.species;
            })) {
            Mon m;
            m.ident = std::format("p{}: {}", side + 1, details.species);
            m.name = details.species;
            m.species = details.species;
            m.level = details.level;
            m.gender = details.gender;
            m.shiny = details.shiny;
            sides_[side].team.push_back(std::move(m));
        }
    } else if (cmd == "-weather") {
        if (a1.empty() || a1 == "none") {
            weather_id_.clear();
            weather_.clear();
        } else if (!l.has("upkeep") || weather_.empty()) {
            weather_id_ = std::string(a1);
            const auto name = lookup(effect_id(a1), "weatherName");
            weather_ = name.empty() ? std::string(effect_name(a1)) : std::string(name);
        }
    } else if (cmd == "-fieldstart") {
        const std::string name(effect_name(a1));
        if (name.empty()) {
            return;
        }
        if (effect_id(a1).ends_with("terrain")) {
            terrain_ = name;
        } else if (std::ranges::none_of(field_other_, [&](const std::string& f) {
                       return to_id(f) == to_id(name);
                   })) {
            field_other_.push_back(name);
        }
    } else if (cmd == "-fieldend") {
        const std::string id = effect_id(a1);
        if (to_id(terrain_) == id) {
            terrain_.clear();
        }
        std::erase_if(field_other_, [&](const std::string& f) {
            return to_id(f) == id;
        });
    } else if (cmd == "-sidestart" && side >= 0) {
        const std::string name(effect_name(a2));
        auto& conditions = sides_[side].conditions;
        const auto it = std::ranges::find_if(conditions, [&](const Condition& c) {
            return to_id(c.name) == to_id(name);
        });
        if (it != conditions.end()) {
            // Spikes and Toxic Spikes come in layers.
            ++it->layers;
        } else if (!name.empty() && conditions.size() < 32) {
            conditions.push_back({name, 1});
        }
    } else if (cmd == "-sideend" && side >= 0) {
        const std::string id = effect_id(a2);
        std::erase_if(sides_[side].conditions, [&](const Condition& c) {
            return to_id(c.name) == id;
        });
    } else if (cmd == "-swapsideconditions") {
        std::swap(sides_[0].conditions, sides_[1].conditions);
    } else if (cmd == "-clearallboost") {
        for (Side& s : sides_) {
            for (Mon& m : s.team) {
                m.boosts.clear();
            }
        }
    }
    // The rest is about a Pokémon.
    Mon* mon = side >= 0 ? find(a1) : nullptr;
    if (!mon) {
        return;
    }
    const auto boost = [](Mon& m, std::string_view stat, int amount) {
        if (stat.empty()) {
            return;
        }
        const std::string key(stat);
        const int value = std::clamp(m.boosts[key] + amount, -6, 6);
        if (value == 0) {
            m.boosts.erase(key);
        } else {
            m.boosts[key] = value;
        }
    };
    if (cmd == "detailschange") {
        const Details details = parse_details(a2);
        if (!details.species.empty()) {
            mon->species = details.species;
            mon->level = details.level;
            mon->gender = details.gender;
            mon->shiny = details.shiny;
            if (!details.tera.empty()) {
                mon->tera = details.tera;
            }
        }
        set_hp(*mon, a3);
    } else if (cmd == "-formechange") {
        if (!trim(a2).empty()) {
            mon->species = std::string(trim(a2));
        }
        set_hp(*mon, a3);
    } else if (cmd == "zchat-types") {
        mon->types = split(a2, '/');
        mon->changed_types.clear();
    } else if (cmd == "move") {
        const std::string name(trim(a2));
        if (!own(side) && from.empty() && !name.empty() && name != "Struggle" && mon->moves.size() < 12 &&
            std::ranges::find(mon->moves, name) == mon->moves.end()) {
            mon->moves.push_back(name);
        }
        sides_[side].last_move = to_id(name);
    } else if (cmd == "cant") {
        reveal(a2, a1);
    } else if (cmd == "faint") {
        mon->hp = 0;
        mon->status = "fnt";
        clear_battle_state(*mon);
    } else if (cmd == "-damage" || cmd == "-heal" || cmd == "-sethp") {
        set_hp(*mon, a2);
    } else if (cmd == "-status") {
        if (mon->status != "fnt") {
            mon->status = std::string(trim(a2));
        }
    } else if (cmd == "-curestatus") {
        if (mon->status != "fnt") {
            mon->status.clear();
        }
    } else if (cmd == "-cureteam") {
        for (Mon& m : sides_[side].team) {
            if (m.status != "fnt") {
                m.status.clear();
            }
        }
    } else if (cmd == "-boost" || cmd == "-unboost") {
        const int amount = std::clamp(to_int(a3), 0, 12);
        boost(*mon, trim(a2), cmd == "-boost" ? amount : -amount);
    } else if (cmd == "-setboost") {
        const std::string key(trim(a2));
        if (!key.empty()) {
            mon->boosts.erase(key);
            boost(*mon, key, std::clamp(to_int(a3), -6, 6));
        }
    } else if (cmd == "-swapboost") {
        if (Mon* other = find(a2); other && other != mon) {
            std::vector<std::string> stats = split(a3, ',');
            if (stats.empty()) {
                stats = {"atk", "def", "spa", "spd", "spe", "accuracy", "evasion"};
            }
            for (const std::string& stat : stats) {
                const int mine = mon->boosts.contains(stat) ? mon->boosts[stat] : 0;
                const int theirs = other->boosts.contains(stat) ? other->boosts[stat] : 0;
                mon->boosts.erase(stat);
                other->boosts.erase(stat);
                boost(*mon, stat, theirs);
                boost(*other, stat, mine);
            }
        }
    } else if (cmd == "-copyboost") {
        if (Mon* target = find(a2)) {
            target->boosts = mon->boosts;
        }
    } else if (cmd == "-clearboost") {
        mon->boosts.clear();
    } else if (cmd == "-clearpositiveboost") {
        std::erase_if(mon->boosts, [](const auto& b) {
            return b.second > 0;
        });
    } else if (cmd == "-clearnegativeboost") {
        std::erase_if(mon->boosts, [](const auto& b) {
            return b.second < 0;
        });
    } else if (cmd == "-invertboost") {
        for (auto& b : mon->boosts) {
            b.second = -b.second;
        }
    } else if (cmd == "-start") {
        reveal(a2, of.empty() ? a1 : of);
        const std::string id = effect_id(a2);
        if (id == "typechange") {
            mon->changed_types = split(a3, '/');
        } else if (id == "typeadd") {
            mon->changed_types = mon->changed_types.empty() ? mon->types : mon->changed_types;
            if (!trim(a3).empty()) {
                mon->changed_types.emplace_back(trim(a3));
            }
        } else if (!id.empty()) {
            std::erase_if(mon->volatiles, [&](const std::string& v) {
                return same_volatile(v, a2);
            });
            if (mon->volatiles.size() < 32) {
                mon->volatiles.push_back(volatile_name(a2));
            }
        }
    } else if (cmd == "-end") {
        std::erase_if(mon->volatiles, [&](const std::string& v) {
            return same_volatile(v, a2);
        });
        if (effect_id(a2) == "typechange") {
            mon->changed_types.clear();
        }
    } else if (cmd == "-item") {
        mon->item = std::string(effect_name(a2));
    } else if (cmd == "-enditem") {
        mon->item.clear();
    } else if (cmd == "-ability") {
        if (!trim(a2).empty()) {
            mon->ability = std::string(effect_name(a2));
        }
    } else if (cmd == "-transform") {
        if (std::ranges::find(mon->volatiles, "Transformed") == mon->volatiles.end()) {
            mon->volatiles.push_back("Transformed");
        }
    } else if (cmd == "-terastallize") {
        mon->tera = std::string(trim(a2));
    } else if (cmd == "-activate") {
        if (starts_with(a2, "ability:")) {
            reveal(a2, a1);
        }
    } else if (cmd == "-mustrecharge") {
        if (std::ranges::find(mon->volatiles, "Must recharge") == mon->volatiles.end()) {
            mon->volatiles.push_back("Must recharge");
        }
    }
    if (cmd == "cant" && trim(a2) == "recharge") {
        std::erase(mon->volatiles, "Must recharge");
    }
}

void Battle::request(std::string_view text) {
    if (side_.empty()) {
        return;
    }
    const auto parsed = json::parse(text);
    if (!parsed || !parsed->is_object()) {
        return;
    }
    const json::Value& r = *parsed;
    const json::Value& side = r["side"];
    if (!side["id"].str().empty() && side["id"].str() != side_) {
        return;
    }
    const int me = side_ == "p2" ? 1 : 0;
    Side& ours = sides_[me];
    bool reviving = false;
    if (side["pokemon"].is_array()) {
        std::vector<Mon> team;
        int active = -1;
        for (const json::Value& p : side["pokemon"].items) {
            const std::string ident = base_ident(p["ident"].str());
            const Details details = parse_details(p["details"].str());
            if (ident.empty() || side_index(ident) != me || details.species.empty() || team.size() >= 24) {
                continue;
            }
            Mon mon;
            mon.ident = ident;
            mon.name = std::string(nickname(ident));
            mon.species = details.species;
            mon.level = details.level;
            mon.gender = details.gender;
            mon.shiny = details.shiny;
            mon.exact = true;
            set_hp(mon, p["condition"].str());
            mon.types = split(p["types"].str(), '/');
            mon.tera_type = std::string(p["teraType"].str());
            mon.tera = std::string(p["terastallized"].str());
            mon.item = std::string(!p["itemName"].str().empty() ? p["itemName"].str() : p["item"].str());
            mon.ability = std::string(!p["abilityName"].str().empty() ? p["abilityName"].str()
                                      : !p["ability"].str().empty()   ? p["ability"].str()
                                                                      : p["baseAbility"].str());
            const json::Value& names = p["moveNames"].is_array() ? p["moveNames"] : p["moves"];
            for (const json::Value& m : names.items) {
                if (!m.str().empty() && mon.moves.size() < 12) {
                    mon.moves.emplace_back(m.str());
                }
            }
            const json::Value& stats = p["stats"];
            for (std::size_t i = 0; i < stats.keys.size() && i < 8; ++i) {
                if (stats.items[i].is_number()) {
                    mon.stats[stats.keys[i]] = stats.items[i].integer();
                }
            }
            // What the request does not tell, the log did. (A fainted Pokémon has no maximum HP, nor Tera type.)
            if (const Mon* old = find(ident)) {
                mon.boosts = old->boosts;
                mon.volatiles = old->volatiles;
                mon.changed_types = old->changed_types;
                if (mon.hp == 0 && old->exact) {
                    mon.maxhp = old->maxhp;
                }
                if (mon.tera.empty()) {
                    mon.tera = old->tera;
                }
            }
            if (mon.hp == 0 && mon.maxhp == 100 && mon.stats.contains("hp")) {
                mon.maxhp = mon.stats["hp"];
            }
            if (p["active"].truthy()) {
                if (active < 0) {
                    active = static_cast<int>(team.size());
                }
                reviving = reviving || p["reviving"].truthy();
            }
            team.push_back(std::move(mon));
        }
        if (!team.empty()) {
            ours.team = std::move(team);
            ours.active = active;
            ours.team_size = static_cast<int>(ours.team.size());
        }
    }
    if (ours.name.empty() && !side["name"].str().empty()) {
        ours.name = std::string(side["name"].str());
    }

    Request q;
    if (r["teamPreview"].truthy()) {
        q.kind = "team";
    } else if (r["forceSwitch"].is_array() && std::ranges::any_of(r["forceSwitch"].items, [](const json::Value& v) {
                   return v.truthy();
               })) {
        q.kind = "switch";
        q.reviving = reviving;
    } else if (r["active"].is_array() && r["active"][0].is_object()) {
        q.kind = "move";
        const json::Value& active = r["active"][0];
        for (const json::Value& m : active["moves"].items) {
            if (!m.is_object() || q.moves.size() >= 24) {
                continue;
            }
            Move move;
            move.name = std::string(m["move"].str());
            move.id = !m["id"].str().empty() ? std::string(m["id"].str()) : to_id(move.name);
            if (move.name.empty()) {
                move.name = move.id;
            }
            move.pp = std::max(0, m["pp"].integer());
            move.maxpp = std::max(0, m["maxpp"].integer());
            move.type = std::string(m["type"].str());
            move.category = std::string(m["category"].str());
            move.base_power = std::max(0, m["basePower"].integer());
            move.accuracy = m["accuracy"].is_number() ? std::clamp(m["accuracy"].integer(), 0, 1000) : 0;
            move.desc = std::string(m["desc"].str());
            move.disabled = m["disabled"].truthy();
            q.moves.push_back(std::move(move));
        }
        q.can_tera = std::string(active["canTerastallize"].str());
        q.trapped = active["trapped"].truthy();
    }
    if (over_ || r["wait"].truthy() || q.kind.empty()) {
        request_.reset();
        answered_ = true;
        error_.clear();
        return;
    }
    q.rqid = r["rqid"].integer(0);
    if (q.rqid <= 0) {
        q.rqid = ++requests_;
    }
    request_ = std::move(q);
    answered_ = false;
    error_.clear();
}

namespace {

    std::string hp_text(int hp, int maxhp, std::string_view status) {
        if (status == "fnt" || hp <= 0) {
            return "fainted";
        }
        const int percent = maxhp > 0 ? std::clamp(static_cast<int>(std::lround(hp * 100.0 / maxhp)), 1, 100) : 0;
        return status.empty() ? std::format("{}%", percent) : std::format("{}% {}", percent, status);
    }

} // namespace

std::vector<std::string> Battle::menu() const {
    std::vector<std::string> lines;
    if (rqid() == 0) {
        return lines;
    }
    const Request& q = *request_;
    if (q.kind == "team") {
        lines.emplace_back("team 123456  bring the team in this order (e.g. team 213456 leads with the second)");
        return lines;
    }
    for (std::size_t i = 0; i < q.moves.size(); ++i) {
        const Move& m = q.moves[i];
        std::string about = m.type;
        if (!m.category.empty()) {
            about += about.empty() ? m.category : ", " + m.category;
        }
        if (m.base_power > 0) {
            about += std::format("{}{} power", about.empty() ? "" : ", ", m.base_power);
        }
        if (m.maxpp > 0) {
            about += std::format("{}{}/{} PP", about.empty() ? "" : ", ", m.pp, m.maxpp);
        }
        lines.push_back(std::format("move {}  {}  {}{}", i + 1, m.name, about, m.disabled ? " (disabled)" : ""));
    }
    if (q.kind == "move" && !q.can_tera.empty()) {
        lines.push_back(
            std::format("move 1 tera  Terastallize into the {} type, with any move (move N tera)", q.can_tera));
    }
    const int me = side_ == "p2" ? 1 : 0;
    const Side& ours = sides_[me];
    if (q.trapped && q.kind == "move") {
        lines.emplace_back("(trapped: no switching)");
        return lines;
    }
    for (std::size_t i = 0; i < ours.team.size(); ++i) {
        const Mon& mon = ours.team[i];
        const bool fainted = mon.status == "fnt" || mon.hp <= 0;
        if (static_cast<int>(i) == ours.active || fainted != q.reviving) {
            continue;
        }
        lines.push_back(std::format("switch {}  {}  {}", i + 1, mon.name,
                                    q.reviving ? "fainted: Revival Blessing brings it back"
                                               : hp_text(mon.hp, mon.maxhp, mon.status)));
    }
    return lines;
}

std::optional<std::string> Battle::parse_choice(std::string_view typed, std::string& error) const {
    error.clear();
    if (rqid() == 0) {
        error = over_ ? "The battle is over." : "There is nothing to choose now.";
        return std::nullopt;
    }
    const Request& q = *request_;
    std::vector<std::string> words = split(lowercase(typed), ' ');
    bool tera = false;
    std::erase_if(words, [&](const std::string& w) {
        if (w == "tera" || w == "terastallize" || w == "terastalize") {
            tera = true;
            return true;
        }
        return false;
    });
    if (words.empty()) {
        error = "Choose a move or a Pokémon (see the menu).";
        return std::nullopt;
    }
    if (q.kind == "team") {
        if (words[0] == "team" && words.size() == 2 && !words[1].empty() && std::ranges::all_of(words[1], [](char c) {
                return c >= '1' && c <= '9';
            })) {
            return "team " + words[1];
        }
        error = "Choose the order of your team, like team 123456.";
        return std::nullopt;
    }
    std::string what;
    if (words[0] == "move" || words[0] == "switch") {
        what = words[0];
        words.erase(words.begin());
    }
    if (what == "move" && q.kind != "move") {
        error = q.reviving ? "Choose a fainted Pokémon to bring back." : "You have to switch.";
        return std::nullopt;
    }
    std::string arg;
    for (const std::string& w : words) {
        arg += arg.empty() ? w : " " + w;
    }
    const int me = side_ == "p2" ? 1 : 0;
    const Side& ours = sides_[me];
    const int number = to_int(arg, 0);
    const std::string arg_id = to_id(arg);

    // Which move or Pokémon: by number, or else by name (moves first, when it could be either).
    int move = 0;
    int pokemon = 0;
    if (what != "switch" && q.kind == "move") {
        if (number > 0) {
            move = number;
        } else if (!arg_id.empty()) {
            for (std::size_t i = 0; i < q.moves.size(); ++i) {
                if (q.moves[i].id == arg_id || to_id(q.moves[i].name) == arg_id) {
                    move = static_cast<int>(i) + 1;
                }
            }
        }
        if (move == 0 && what == "move") {
            error = std::format("You have no move {}.", arg);
            return std::nullopt;
        }
    }
    if (move == 0) {
        if (number > 0) {
            pokemon = number;
        } else if (!arg_id.empty()) {
            for (std::size_t i = 0; i < ours.team.size(); ++i) {
                if (to_id(ours.team[i].name) == arg_id || to_id(ours.team[i].species) == arg_id) {
                    pokemon = static_cast<int>(i) + 1;
                }
            }
        }
        if (pokemon == 0) {
            error = std::format("There is no move nor Pokémon {} to choose.", arg);
            return std::nullopt;
        }
    }

    if (move > 0) {
        if (move > static_cast<int>(q.moves.size())) {
            error = std::format("There is no move {}: choose 1 to {}.", move, q.moves.size());
            return std::nullopt;
        }
        const Move& m = q.moves[move - 1];
        if (m.disabled) {
            error = std::format("{} is disabled now.", m.name);
            return std::nullopt;
        }
        if (tera && q.can_tera.empty()) {
            error = "You cannot Terastallize now.";
            return std::nullopt;
        }
        return std::format("move {}{}", move, tera ? " terastallize" : "");
    }
    if (pokemon > static_cast<int>(ours.team.size())) {
        error = std::format("There is no Pokémon {}: choose 1 to {}.", pokemon, ours.team.size());
        return std::nullopt;
    }
    const Mon& mon = ours.team[pokemon - 1];
    const bool fainted = mon.status == "fnt" || mon.hp <= 0;
    if (tera) {
        error = "Terastallizing goes with a move, not a switch.";
        return std::nullopt;
    }
    if (q.reviving) {
        if (!fainted) {
            error = std::format("{} has not fainted: choose a fainted Pokémon to revive.", mon.name);
            return std::nullopt;
        }
        return std::format("switch {}", pokemon);
    }
    if (pokemon - 1 == ours.active && !(q.kind == "switch" && fainted)) {
        error = std::format("{} is already in battle.", mon.name);
        return std::nullopt;
    }
    if (fainted) {
        error = std::format("{} has fainted.", mon.name);
        return std::nullopt;
    }
    if (pokemon - 1 == ours.active) {
        error = std::format("{} is already in battle.", mon.name);
        return std::nullopt;
    }
    if (q.trapped && q.kind == "move") {
        error = "You are trapped: you cannot switch out.";
        return std::nullopt;
    }
    return std::format("switch {}", pokemon);
}

std::string Battle::json(std::string_view extra) const {
    std::string out = "{";
    Writer w(out);
    if (!trim(extra).empty()) {
        w.comma();
        out += extra;
    }
    w.key("you").string(side_);
    w.key("turn").number(turn_);
    w.key("over").boolean(over_);
    w.key("winner").string(over_ ? winner_ : std::string());
    w.key("sides");
    out += '[';
    const int me = side_ == "p2" ? 1 : 0;
    for (int i = 0; i < 2; ++i) {
        const Side& s = sides_[i];
        const bool ours = !side_.empty() && i == me;
        if (i > 0) {
            out += ',';
        }
        out += '{';
        Writer sw(out);
        sw.key("id").string(i == 0 ? "p1" : "p2");
        sw.key("name").string(s.name);
        sw.key("teamSize").number(s.team_size);
        sw.key("active").number(s.active >= 0 && s.active < static_cast<int>(s.team.size()) ? s.active : -1);
        sw.key("team");
        out += '[';
        for (std::size_t k = 0; k < s.team.size(); ++k) {
            const Mon& m = s.team[k];
            if (k > 0) {
                out += ',';
            }
            out += '{';
            Writer mw(out);
            mw.key("name").string(m.name);
            mw.key("species").string(m.species);
            mw.key("level").number(m.level);
            mw.key("gender").string(m.gender);
            mw.key("shiny").boolean(m.shiny);
            mw.key("hp").number(m.hp);
            mw.key("maxhp").number(m.maxhp);
            mw.key("exact").boolean(ours);
            mw.key("status").string(m.status);
            mw.key("types").raw(string_array(m.changed_types.empty() ? m.types : m.changed_types));
            mw.key("tera").string(m.tera);
            mw.key("teraType").string(ours ? m.tera_type : std::string());
            mw.key("boosts").raw(int_object(m.boosts));
            mw.key("volatiles").raw(string_array(m.volatiles));
            mw.key("item").string(m.item);
            mw.key("ability").string(m.ability);
            mw.key("moves").raw(string_array(m.moves));
            mw.key("stats").raw(ours ? int_object(m.stats) : "{}");
            out += '}';
        }
        out += ']';
        std::vector<std::string> conditions;
        for (const Condition& c : s.conditions) {
            conditions.push_back(c.layers > 1 ? std::format("{} ({})", c.name, c.layers) : c.name);
        }
        sw.key("conditions").raw(string_array(conditions));
        out += '}';
    }
    out += ']';
    w.key("field");
    out += '{';
    {
        Writer fw(out);
        fw.key("weather").string(weather_);
        fw.key("terrain").string(terrain_);
        fw.key("other").raw(string_array(field_other_));
    }
    out += '}';
    w.key("request");
    if (rqid() == 0) {
        out += "null";
    } else {
        const Request& q = *request_;
        out += '{';
        Writer rw(out);
        rw.key("rqid").number(q.rqid);
        rw.key("kind").string(q.kind);
        rw.key("moves");
        out += '[';
        for (std::size_t i = 0; i < q.moves.size(); ++i) {
            const Move& m = q.moves[i];
            if (i > 0) {
                out += ',';
            }
            out += '{';
            Writer mw(out);
            mw.key("slot").number(static_cast<int>(i) + 1);
            mw.key("name").string(m.name);
            mw.key("id").string(m.id);
            mw.key("pp").number(m.pp);
            mw.key("maxpp").number(m.maxpp);
            mw.key("type").string(m.type);
            mw.key("category").string(m.category);
            mw.key("basePower").number(m.base_power);
            mw.key("accuracy");
            if (m.accuracy > 0) {
                mw.number(m.accuracy);
            } else {
                mw.boolean(true);
            }
            mw.key("desc").string(m.desc);
            mw.key("disabled").boolean(m.disabled);
            out += '}';
        }
        out += ']';
        rw.key("canTera").string(q.can_tera);
        rw.key("trapped").boolean(q.trapped);
        rw.key("reviving").boolean(q.reviving);
        rw.key("switches");
        out += '[';
        const Side& ours = sides_[me];
        for (std::size_t i = 0; i < ours.team.size(); ++i) {
            const Mon& m = ours.team[i];
            if (i > 0) {
                out += ',';
            }
            out += '{';
            Writer sw(out);
            sw.key("slot").number(static_cast<int>(i) + 1);
            sw.key("name").string(m.name);
            sw.key("species").string(m.species);
            sw.key("hp").number(m.hp);
            sw.key("maxhp").number(m.maxhp);
            sw.key("status").string(m.status);
            sw.key("fainted").boolean(m.status == "fnt" || m.hp <= 0);
            sw.key("active").boolean(static_cast<int>(i) == ours.active);
            out += '}';
        }
        out += ']';
        out += '}';
    }
    w.key("error").string(rqid() != 0 ? error_ : std::string());
    w.key("log");
    out += '[';
    for (std::size_t i = 0; i < log_.size(); ++i) {
        static constexpr std::string_view kinds[] = {"turn", "move",   "text",  "minor",
                                                     "chat", "notice", "error", "result"};
        const LogLine& line = log_[i];
        if (i > 0) {
            out += ',';
        }
        out += '{';
        Writer lw(out);
        lw.key("k").string(kinds[static_cast<int>(line.kind)]);
        lw.key("t").string(line.text);
        if (line.kind == LogLine::Kind::Chat) {
            lw.key("n").string(line.name);
            lw.key("c").string(line.color);
        }
        out += '}';
    }
    out += ']';
    out += '}';
    return out;
}

} // namespace zchat::pokemon

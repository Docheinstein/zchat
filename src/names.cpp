#include "names.hpp"

#include <array>
#include <string_view>

namespace zchat {

namespace {

    constexpr std::array adjectives = std::to_array<std::string_view>({
        "Angry",  "Atomic",   "Bouncy",  "Brave",  "Cheeky",   "Cosmic",   "Cranky",    "Crispy", "Dapper",
        "Dizzy",  "Electric", "Epic",    "Fancy",  "Fearless", "Fluffy",   "Funky",     "Fuzzy",  "Galactic",
        "Giant",  "Glorious", "Grumpy",  "Hyper",  "Jolly",    "Jumpy",    "Legendary", "Lucky",  "Mighty",
        "Mystic", "Neon",     "Nimble",  "Noble",  "Quantum",  "Radical",  "Rusty",     "Sassy",  "Shiny",
        "Sleepy", "Sneaky",   "Sparkly", "Speedy", "Spicy",    "Stealthy", "Stormy",    "Swift",  "Thunder",
        "Turbo",  "Wacky",    "Wild",    "Witty",  "Zany",     "Zen",      "Zesty",
    });

    constexpr std::array dinosaurs = std::to_array<std::string_view>({
        "Allosaurus",      "Ankylosaurus",  "Apatosaurus",    "Archaeopteryx",  "Baryonyx",           "Brachiosaurus",
        "Brontosaurus",    "Carnotaurus",   "Ceratosaurus",   "Compsognathus",  "Deinonychus",        "Dilophosaurus",
        "Diplodocus",      "Edmontosaurus", "Gallimimus",     "Giganotosaurus", "Iguanodon",          "Kentrosaurus",
        "Maiasaura",       "Megalosaurus",  "Microraptor",    "Oviraptor",      "Pachycephalosaurus", "Parasaurolophus",
        "Protoceratops",   "Pteranodon",    "Quetzalcoatlus", "Spinosaurus",    "Stegosaurus",        "Styracosaurus",
        "Therizinosaurus", "Triceratops",   "Troodon",        "Tyrannosaurus",  "Utahraptor",         "Velociraptor",
    });

    template <typename Array>
    std::string_view pick(const Array& items, std::mt19937_64& rng) {
        std::uniform_int_distribution<std::size_t> dist(0, items.size() - 1);
        return items[dist(rng)];
    }

} // namespace

std::string random_name(std::mt19937_64& rng) {
    std::string name(pick(adjectives, rng));
    name += ' ';
    name += pick(dinosaurs, rng);
    return name;
}

} // namespace zchat

// Bowling: a lane's ball and pins, their physics on the lane's plane, and the rules of a game of ten frames.
//
// A lane's coordinates are x across it (0 in the middle, to +-layout_lane_half) and d along it, from the foul line
// (0) to the pins (the head pin at layout_headpin). Pins and the ball are circles on that plane that push each other
// (the ball much harder: it weighs more); a pin hit falls over, in the way it was pushed, and keeps sliding a bit,
// knocking others. Who plays a lane simulates it, and tells the others how it is (see encode_lane()).

#include "hub_math.h"
#include "layout.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr float ball_radius = 0.108f;
constexpr float pin_radius = 0.06f;
constexpr float ball_mass = 7.0f;
constexpr float pin_mass = 1.6f;
constexpr float lane_surface = 0.07f;
constexpr int frames = 10;

struct Pin {
    float x = 0, d = 0;
    float vx = 0, vd = 0;
    // How far it fell, 0 (standing) to 1 (lying), and which way.
    float tilt = 0;
    float fall_x = 0, fall_d = 1;
    bool knocked = false;
    // Off the lane (into the gutter or the pit), or swept away after the ball.
    bool gone = false;
    // Counted in a roll already.
    bool counted = false;
};

struct Ball {
    float x = 0, d = -0.4f;
    float vx = 0, vd = 0;
    bool rolling = false;
    bool gutter = false;
    // How much it turned, for drawing it rolling.
    float roll = 0;
};

enum class LaneState { Free, Aiming, Rolling, Over };

struct Lane {
    LaneState state = LaneState::Free;
    // Who plays it: us, or another (by id); and their name.
    bool mine = false;
    std::string owner;
    std::string owner_name;
    double heard = 0;
    // The throw being prepared: where across the lane, at what angle, how hard (while Space is held, it swings).
    float aim_x = 0;
    float aim_angle = 0;
    float power = 0;
    bool charging = false;
    float charge = 0;
    Ball ball;
    Pin pins[10];
    std::vector<int> rolls;
    int frame = 0;
    int ball_in_frame = 0;
    float settle = 0;
    float rolling_time = 0;
    // What the last roll did ("Strike!", "Spare!", "7"), for a while.
    std::string news;
    double news_at = -100;
};

static void reset_pins(Lane& lane) {
    int k = 0;
    for (int row = 0; row < 4; ++row) {
        for (int i = 0; i <= row; ++i) {
            Pin& p = lane.pins[k++];
            p = {};
            p.x = (static_cast<float>(i) - static_cast<float>(row) / 2) * layout_pin_spacing;
            p.d = layout_headpin + static_cast<float>(row) * layout_pin_row;
        }
    }
}

static void ready_ball(Lane& lane) {
    lane.ball = {};
    lane.ball.x = lane.aim_x;
    lane.power = 0;
    lane.charge = 0;
    lane.charging = false;
}

static void start_game(Lane& lane) {
    lane.rolls.clear();
    lane.frame = 0;
    lane.ball_in_frame = 0;
    lane.aim_x = 0;
    lane.aim_angle = 0;
    lane.news.clear();
    reset_pins(lane);
    ready_ball(lane);
    lane.state = LaneState::Aiming;
}

static void throw_ball(Lane& lane) {
    // Power 0 to 1: 5 to 9.5 meters a second, a slow roll to a fast one.
    const float speed = 5.0f + 4.5f * lane.power;
    lane.ball.x = lane.aim_x;
    lane.ball.d = -0.4f;
    lane.ball.vx = std::sin(lane.aim_angle) * speed;
    lane.ball.vd = std::cos(lane.aim_angle) * speed;
    lane.ball.rolling = true;
    lane.settle = 0;
    lane.rolling_time = 0;
    lane.state = LaneState::Rolling;
}

static void knock(Pin& p) {
    if (!p.knocked) {
        p.knocked = true;
        const float speed = std::sqrt(p.vx * p.vx + p.vd * p.vd);
        if (speed > 0.001f) {
            p.fall_x = p.vx / speed;
            p.fall_d = p.vd / speed;
        }
    }
}

// Two circles pushing each other apart, as an elastic-ish collision. Returns how hard they met.
static float collide(float& ax, float& ad, float& avx, float& avd, float am, float& bx, float& bd, float& bvx,
                     float& bvd, float bm, float distance) {
    const float dx = bx - ax, dd = bd - ad;
    const float dist = std::sqrt(dx * dx + dd * dd);
    if (dist >= distance || dist <= 0.0001f) {
        return 0;
    }
    const float nx = dx / dist, nd = dd / dist;
    const float closing = (avx - bvx) * nx + (avd - bvd) * nd;
    // Apart, by their weights.
    const float overlap = distance - dist;
    ax -= nx * overlap * bm / (am + bm);
    ad -= nd * overlap * bm / (am + bm);
    bx += nx * overlap * am / (am + bm);
    bd += nd * overlap * am / (am + bm);
    if (closing <= 0) {
        return 0;
    }
    const float restitution = 0.6f;
    const float j = (1 + restitution) * closing / (1 / am + 1 / bm);
    avx -= j / am * nx;
    avd -= j / am * nd;
    bvx += j / bm * nx;
    bvd += j / bm * nd;
    return closing;
}

static int standing(const Lane& lane) {
    int n = 0;
    for (const Pin& p : lane.pins) {
        n += !p.knocked && !p.gone;
    }
    return n;
}

// A roll is over: how many it knocked down, and what comes next.
static void finish_roll(Lane& lane, double now) {
    int knocked = 0;
    for (Pin& p : lane.pins) {
        if (p.knocked && !p.counted) {
            ++knocked;
            p.counted = true;
        }
    }
    lane.rolls.push_back(knocked);
    const int left = standing(lane);
    bool reset = false;
    bool over = false;
    if (lane.frame < frames - 1) {
        if (lane.ball_in_frame == 0 && left > 0) {
            lane.ball_in_frame = 1;
        } else {
            ++lane.frame;
            lane.ball_in_frame = 0;
            reset = true;
        }
    } else if (lane.ball_in_frame == 0) {
        // The tenth frame: a strike gives two more balls, a spare one more.
        lane.ball_in_frame = 1;
        reset = left == 0;
    } else if (lane.ball_in_frame == 1) {
        const int first = lane.rolls[lane.rolls.size() - 2];
        if (first == 10 || first + knocked == 10) {
            lane.ball_in_frame = 2;
            reset = left == 0;
        } else {
            over = true;
        }
    } else {
        over = true;
    }
    // A full rack all down at once is a strike; the rest of one, a spare.
    if (left == 0) {
        lane.news = left + knocked == 10 ? "Strike!" : "Spare!";
    } else {
        lane.news = knocked == 0 ? "Gutter..." : std::to_string(knocked);
    }
    lane.news_at = now;
    if (reset) {
        reset_pins(lane);
    } else {
        // The fallen pins are swept away.
        for (Pin& p : lane.pins) {
            p.gone = p.gone || p.knocked;
        }
    }
    ready_ball(lane);
    lane.state = over ? LaneState::Over : LaneState::Aiming;
}

static void step_lane(Lane& lane, float dt, double now) {
    if (lane.state != LaneState::Rolling) {
        return;
    }
    lane.rolling_time += dt;
    constexpr int steps = 8;
    const float h = dt / steps;
    Ball& b = lane.ball;
    for (int s = 0; s < steps; ++s) {
        if (b.rolling) {
            b.x += b.vx * h;
            b.d += b.vd * h;
            b.vx *= 1 - 0.05f * h;
            b.vd *= 1 - 0.05f * h;
            b.roll += std::sqrt(b.vx * b.vx + b.vd * b.vd) * h / ball_radius;
            if (!b.gutter && std::abs(b.x) > layout_lane_half) {
                // Into the gutter, where it rolls straight to the pit.
                b.gutter = true;
                b.x = (b.x > 0 ? 1 : -1) * (layout_lane_half + layout_gutter / 2);
                b.vx = 0;
            }
            if (b.d > layout_lane_end + 0.4f || b.vd < 0.05f) {
                b.rolling = false;
            }
        }
        for (Pin& p : lane.pins) {
            if (p.gone || !p.knocked) {
                continue;
            }
            p.x += p.vx * h;
            p.d += p.vd * h;
            // Sliding on its side slows it down.
            const float speed = std::sqrt(p.vx * p.vx + p.vd * p.vd);
            const float slowed = std::max(0.0f, speed - 2.5f * h);
            if (speed > 0) {
                p.vx *= slowed / speed;
                p.vd *= slowed / speed;
            }
            p.tilt = std::min(1.0f, p.tilt + h * 4.0f);
            if (std::abs(p.x) > layout_lane_half + 0.05f || p.d > layout_lane_end + 0.2f) {
                p.gone = true;
            }
        }
        if (b.rolling && !b.gutter) {
            for (Pin& p : lane.pins) {
                if (p.gone || p.tilt > 0.7f) {
                    continue;
                }
                if (collide(b.x, b.d, b.vx, b.vd, ball_mass, p.x, p.d, p.vx, p.vd, pin_mass,
                            ball_radius + pin_radius) > 0.05f) {
                    knock(p);
                }
            }
        }
        for (int i = 0; i < 10; ++i) {
            for (int j = i + 1; j < 10; ++j) {
                Pin& a = lane.pins[i];
                Pin& c = lane.pins[j];
                if (a.gone || c.gone || (!a.knocked && !c.knocked)) {
                    continue;
                }
                // A lying pin is long: it reaches further than a standing one.
                const float reach = pin_radius * 2 + 0.12f * std::max(a.tilt, c.tilt);
                if (collide(a.x, a.d, a.vx, a.vd, pin_mass, c.x, c.d, c.vx, c.vd, pin_mass, reach) > 0.15f) {
                    knock(a);
                    knock(c);
                }
            }
        }
    }
    // Over when the ball is gone and the pins are still, for a moment (or after a while anyway).
    bool still = !b.rolling;
    for (const Pin& p : lane.pins) {
        if (!p.gone && p.knocked && (p.tilt < 1 || p.vx * p.vx + p.vd * p.vd > 0.003f)) {
            still = false;
        }
    }
    lane.settle = still ? lane.settle + dt : 0;
    if (lane.settle > 0.7f || lane.rolling_time > 9) {
        finish_roll(lane, now);
    }
}

// The scores: the total after each frame (-1 for a frame not over yet, waiting for its rolls or bonus).
static void score(const std::vector<int>& rolls, int totals[frames]) {
    const auto at = [&](std::size_t i) {
        return i < rolls.size() ? rolls[i] : -1;
    };
    int total = 0;
    std::size_t i = 0;
    for (int f = 0; f < frames; ++f) {
        totals[f] = -1;
    }
    for (int f = 0; f < frames && i < rolls.size(); ++f) {
        if (at(i) == 10) {
            if (at(i + 1) < 0 || at(i + 2) < 0) {
                break;
            }
            total += 10 + at(i + 1) + at(i + 2);
            i += 1;
        } else {
            if (at(i + 1) < 0) {
                break;
            }
            if (at(i) + at(i + 1) == 10) {
                if (at(i + 2) < 0) {
                    break;
                }
                total += 10 + at(i + 2);
            } else {
                total += at(i) + at(i + 1);
            }
            i += 2;
        }
        totals[f] = total;
    }
}

// The rolls as a scoresheet shows them: "X", "7 /", "9 -", one string per frame.
static std::vector<std::string> marks(const std::vector<int>& rolls) {
    std::vector<std::string> out;
    const auto mark = [](int n) {
        return n == 0 ? std::string("-") : std::to_string(n);
    };
    std::size_t i = 0;
    for (int f = 0; f < frames && i < rolls.size(); ++f) {
        if (f < frames - 1) {
            if (rolls[i] == 10) {
                out.push_back("X");
                i += 1;
            } else if (i + 1 < rolls.size()) {
                out.push_back(mark(rolls[i]) + (rolls[i] + rolls[i + 1] == 10 ? " /" : " " + mark(rolls[i + 1])));
                i += 2;
            } else {
                out.push_back(mark(rolls[i]));
                i += 1;
            }
            continue;
        }
        // The tenth: up to three, each strike an X and each spare a /.
        std::string s;
        int prev = -1;
        for (int k = 0; i < rolls.size() && k < 3; ++k, ++i) {
            const int n = rolls[i];
            const bool spare = prev >= 0 && prev != 10 && prev + n == 10;
            s += (s.empty() ? "" : " ") + (spare ? std::string("/") : n == 10 ? std::string("X") : mark(n));
            prev = spare || n == 10 ? -1 : n;
        }
        out.push_back(s);
    }
    return out;
}

static std::string scoreboard(const Lane& lane) {
    int totals[frames];
    score(lane.rolls, totals);
    int total = 0;
    for (const int t : totals) {
        total = t >= 0 ? t : total;
    }
    std::string line;
    for (const auto& m : marks(lane.rolls)) {
        line += (line.empty() ? "" : "  |  ") + m;
    }
    const std::string where = lane.state == LaneState::Over
                                  ? std::string("Game over")
                                  : "Frame " + std::to_string(lane.frame + 1) +
                                        (lane.ball_in_frame > 0 ? ", ball " + std::to_string(lane.ball_in_frame + 1)
                                                                : std::string());
    return where + (line.empty() ? std::string() : "     " + line) + "     Total " + std::to_string(total);
}

// How a lane is, for the others: "STATE FRAME BALL AIMX BALLX BALLD ROLLING GUTTER ROLL N ROLLS... PINS..." with
// each pin "X D TILT FALLX FALLD GONE".
static std::string encode_lane(const Lane& lane) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d %d %d %.3f %.3f %.3f %d %d %.2f %zu", static_cast<int>(lane.state), lane.frame,
                  lane.ball_in_frame, lane.aim_x, lane.ball.x, lane.ball.d, lane.ball.rolling ? 1 : 0,
                  lane.ball.gutter ? 1 : 0, lane.ball.roll, lane.rolls.size());
    std::string out = buf;
    for (const int r : lane.rolls) {
        out += " " + std::to_string(r);
    }
    for (const Pin& p : lane.pins) {
        std::snprintf(buf, sizeof buf, " %.3f %.3f %.2f %.2f %.2f %d", p.x, p.d, p.tilt, p.fall_x, p.fall_d,
                      p.gone ? 1 : 0);
        out += buf;
    }
    return out;
}

static bool decode_lane(const std::string& text, Lane& lane) {
    const char* s = text.c_str();
    char* end = nullptr;
    const auto number = [&](float& out) {
        out = std::strtof(s, &end);
        if (end == s) {
            return false;
        }
        s = end;
        return true;
    };
    float v[10];
    for (float& x : v) {
        if (!number(x)) {
            return false;
        }
    }
    const int state = static_cast<int>(v[0]);
    const int count = static_cast<int>(v[9]);
    if (state < 0 || state > 3 || count < 0 || count > 21) {
        return false;
    }
    std::vector<int> rolls;
    for (int i = 0; i < count; ++i) {
        float r = 0;
        if (!number(r) || r < 0 || r > 10) {
            return false;
        }
        rolls.push_back(static_cast<int>(r));
    }
    Pin pins[10];
    for (Pin& p : pins) {
        float f[6];
        for (float& x : f) {
            if (!number(x)) {
                return false;
            }
        }
        p.x = f[0];
        p.d = f[1];
        p.tilt = clampf(f[2], 0, 1);
        p.fall_x = f[3];
        p.fall_d = f[4];
        p.gone = f[5] != 0;
        p.knocked = p.tilt > 0;
    }
    lane.state = static_cast<LaneState>(state);
    lane.frame = static_cast<int>(v[1]);
    lane.ball_in_frame = static_cast<int>(v[2]);
    lane.aim_x = v[3];
    lane.ball.x = v[4];
    lane.ball.d = v[5];
    lane.ball.rolling = v[6] != 0;
    lane.ball.gutter = v[7] != 0;
    lane.ball.roll = v[8];
    lane.rolls = std::move(rolls);
    for (int i = 0; i < 10; ++i) {
        lane.pins[i] = pins[i];
    }
    return true;
}

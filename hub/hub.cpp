// The hub: a little 3D world where everybody in the chat walks around as a crewmate of their color, says things
// over their head, and goes bowling. zchat starts it (/hub) and is its line to the others: they talk in lines of text,
// on our standard input and output.
//
// From zchat:
//   me ID R G B NAME                       who we are (id in hex, color 0-255)
//   at ID R G B X Z YAW ANIM LANE NAME     where someone else is (ANIM 0 still, 1 walking, 2 running; LANE -1, 0, 1)
//   gone ID                                they left the hub
//   say ID TEXT                            somebody said something in #general (us too): over their head
//   bowl ID LANE STATE...                  how their game on a lane is (see encode_lane())
//   quit                                   zchat is closing
// To zchat:
//   at X Z YAW ANIM LANE                   where we are: when it changes, and every second
//   say TEXT                               said in #general
//   bowl LANE STATE...                     how our game is, while we play
//
// Without zchat (started on its own) the hub is offline, to try it out alone.

#include "hub_bowling.cpp"
#include "hub_math.h"
#include "hub_platform.h"
#include "hub_render.cpp"
#include "layout.h"

#include "ball_mesh.hpp"
#include "crewmate_mesh.hpp"
#include "pin_mesh.hpp"
#include "plaza_mesh.hpp"
#include "shadow_mesh.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

constexpr float player_radius = 0.35f;
constexpr float walk_speed = 3.4f;
constexpr float run_speed = 6.5f;
constexpr float bubble_life = 8.0f;
constexpr double send_interval = 1.0 / 12;
constexpr double heartbeat = 1.0;
constexpr double lane_send_interval = 1.0 / 15;
// Who is not heard from for this long is gone (zchat says so too, this is in case it does not).
constexpr double timeout = 6.0;

struct Player {
    std::string id;
    std::string name;
    V3 color = {0.85f, 0.85f, 0.9f};
    // Where they are, and where they are going (the others: their last told place, approached smoothly).
    float x = 0, z = 0, y = 0, yaw = 0;
    float tx = 0, tz = 0, tyaw = 0;
    int anim = 0;
    int lane = -1;
    float walk = 0;
    double heard = 0;
    std::string bubble;
    double bubble_at = -100;
};

struct CachedText {
    Texture texture;
    std::uint64_t used = 0;
};

struct Hub {
    bool ready = false;
    Renderer r;
    Mesh plaza, crewmate, pin, ball, shadow;

    Player me;
    float vy = 0;
    bool known = false;
    std::vector<Player> others;

    // The camera, around us: its angle around the vertical (0: looking toward -z), how much it looks down, how far.
    float cam_yaw = 0;
    float cam_pitch = 0.32f;
    float cam_distance = 7.0f;
    V3 cam_pos;

    // Typing a message.
    bool typing = false;
    std::string draft;

    Lane lanes[2];
    int my_lane = -1;

    std::string last_sent;
    double sent_at = -100;
    double lane_sent_at = -100;
    std::map<std::string, CachedText> texts;
    std::uint64_t frame = 0;
};

// --- Text: drawn by the platform once, kept as textures.

static const Texture& text_texture(Hub& hub, const std::string& text, int size, bool bold) {
    const std::string key = std::to_string(size) + (bold ? "b" : "r") + text;
    auto it = hub.texts.find(key);
    if (it == hub.texts.end()) {
        // The oldest go when there are many.
        if (hub.texts.size() > 300) {
            for (auto i = hub.texts.begin(); i != hub.texts.end();) {
                if (hub.frame - i->second.used > 120) {
                    free_texture(i->second.texture);
                    i = hub.texts.erase(i);
                } else {
                    ++i;
                }
            }
        }
        TextBitmap bitmap;
        CachedText cached;
        if (platform_text(text, size, bold, bitmap) && bitmap.width > 0) {
            cached.texture = make_texture(bitmap.width, bitmap.height, bitmap.rgba.data());
        }
        it = hub.texts.emplace(key, cached).first;
    }
    it->second.used = hub.frame;
    return it->second.texture;
}

// Text at a point: align 0 left, 1 centered, 2 right, of x. Returns its width.
static float draw_text(Hub& hub, const std::string& text, float x, float y, int size, V4 color, int align = 0,
                       bool bold = false, bool backdrop = false) {
    if (text.empty()) {
        return 0;
    }
    const Texture& t = text_texture(hub, text, size, bold);
    const float w = static_cast<float>(t.width), h = static_cast<float>(t.height);
    const float left = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    if (backdrop) {
        const float pad = static_cast<float>(size) * 0.35f;
        draw_rect(hub.r, left - pad, y - pad * 0.4f, w + 2 * pad, h + pad * 0.8f, {0.08f, 0.09f, 0.12f, 0.62f * color.w});
    }
    draw_quad(hub.r, left, y, w, h, t, color);
    return w;
}

// --- Talking to zchat.

static std::vector<std::string> split(const std::string& line, std::size_t fields, std::string& rest) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (out.size() < fields && at <= line.size()) {
        const std::size_t space = line.find(' ', at);
        out.push_back(line.substr(at, space == std::string::npos ? std::string::npos : space - at));
        at = space == std::string::npos ? line.size() + 1 : space + 1;
    }
    rest = at < line.size() ? line.substr(at) : std::string();
    return out;
}

static float number(const std::string& s) {
    return std::strtof(s.c_str(), nullptr);
}

static Player* other(Hub& hub, const std::string& id) {
    for (Player& p : hub.others) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

static Player* anyone(Hub& hub, const std::string& id) {
    return id == hub.me.id ? &hub.me : other(hub, id);
}

static V3 rgb(const std::string& r, const std::string& g, const std::string& b) {
    return {number(r) / 255, number(g) / 255, number(b) / 255};
}

static void receive(Hub& hub, const std::string& line, double now) {
    std::string rest;
    const auto f = split(line, 1, rest);
    const std::string& verb = f[0];
    if (verb == "me") {
        const auto a = split(rest, 4, rest);
        if (a.size() == 4) {
            hub.me.id = a[0];
            hub.me.color = rgb(a[1], a[2], a[3]);
            hub.me.name = rest;
            hub.known = true;
        }
    } else if (verb == "at") {
        const auto a = split(rest, 9, rest);
        if (a.size() < 9 || a[0] == hub.me.id) {
            return;
        }
        Player* p = other(hub, a[0]);
        if (!p) {
            hub.others.push_back({});
            p = &hub.others.back();
            p->id = a[0];
            p->x = p->tx = number(a[4]);
            p->z = p->tz = number(a[5]);
            p->yaw = p->tyaw = number(a[6]);
        }
        p->color = rgb(a[1], a[2], a[3]);
        p->tx = number(a[4]);
        p->tz = number(a[5]);
        p->tyaw = number(a[6]);
        p->anim = static_cast<int>(number(a[7]));
        p->lane = static_cast<int>(number(a[8]));
        p->name = rest;
        p->heard = now;
    } else if (verb == "gone") {
        std::erase_if(hub.others, [&](const Player& p) {
            return p.id == rest;
        });
        for (Lane& lane : hub.lanes) {
            if (!lane.mine && lane.owner == rest) {
                lane = {};
            }
        }
    } else if (verb == "say") {
        const auto a = split(rest, 1, rest);
        if (Player* p = anyone(hub, a[0])) {
            p->bubble = rest.size() > 120 ? rest.substr(0, 117) + "..." : rest;
            p->bubble_at = now;
        }
    } else if (verb == "bowl") {
        const auto a = split(rest, 2, rest);
        const int index = a.size() == 2 ? static_cast<int>(number(a[1])) : -1;
        if (index < 0 || index > 1 || a[0] == hub.me.id || hub.lanes[index].mine) {
            return;
        }
        Lane& lane = hub.lanes[index];
        if (decode_lane(rest, lane)) {
            lane.owner = a[0];
            const Player* p = other(hub, a[0]);
            lane.owner_name = p ? p->name : std::string("someone");
            lane.heard = now;
        }
    } else if (verb == "quit") {
        platform_quit();
    }
}

static void send_place(Hub& hub, double now) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "at %.2f %.2f %.2f %d %d", hub.me.x, hub.me.z, hub.me.yaw, hub.me.anim,
                  hub.my_lane);
    const std::string line = buf;
    if (line != hub.last_sent ? now - hub.sent_at >= send_interval : now - hub.sent_at >= heartbeat) {
        platform_send(line);
        hub.last_sent = line;
        hub.sent_at = now;
    }
}

// --- Walking.

static bool lane_taken(const Hub& hub, int index) {
    if (hub.lanes[index].mine) {
        return false;
    }
    if (!hub.lanes[index].owner.empty()) {
        return true;
    }
    for (const Player& p : hub.others) {
        if (p.lane == index) {
            return true;
        }
    }
    return false;
}

static void collide_world(float& x, float& z) {
    for (const auto& c : layout_circles) {
        const float dx = x - c[0], dz = z - c[1];
        const float dist = std::sqrt(dx * dx + dz * dz), reach = c[2] + player_radius;
        if (dist < reach && dist > 0.0001f) {
            x = c[0] + dx / dist * reach;
            z = c[1] + dz / dist * reach;
        }
    }
    for (const auto& b : layout_boxes) {
        const float cx = clampf(x, b[0], b[2]), cz = clampf(z, b[1], b[3]);
        const float dx = x - cx, dz = z - cz;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist > 0.0001f && dist < player_radius) {
            x = cx + dx / dist * player_radius;
            z = cz + dz / dist * player_radius;
        } else if (dist <= 0.0001f) {
            // Inside: out by the nearest side.
            const float out[4] = {x - b[0], b[2] - x, z - b[1], b[3] - z};
            const int side = static_cast<int>(std::min_element(out, out + 4) - out);
            if (side == 0) {
                x = b[0] - player_radius;
            } else if (side == 1) {
                x = b[2] + player_radius;
            } else if (side == 2) {
                z = b[1] - player_radius;
            } else {
                z = b[3] + player_radius;
            }
        }
    }
    const float r = std::sqrt(x * x + z * z), edge = layout_world_radius - player_radius;
    if (r > edge) {
        x *= edge / r;
        z *= edge / r;
    }
}

// Where to stand to bowl on a lane, and how near counts.
static V3 lane_spot(int index) {
    return {layout_lane_x[index], 0, layout_foul_z + 1.0f};
}

static int lane_nearby(const Hub& hub) {
    for (int i = 0; i < 2; ++i) {
        const V3 s = lane_spot(i);
        const float dx = hub.me.x - s.x, dz = hub.me.z - s.z;
        if (dx * dx + dz * dz < 1.3f * 1.3f) {
            return i;
        }
    }
    return -1;
}

static void walk(Hub& hub, const Input& in) {
    Player& me = hub.me;
    float fx = 0, fz = 0;
    if (!hub.typing) {
        fz = (in.keys[key_w].down || in.keys[key_up].down ? 1.0f : 0.0f) -
             (in.keys[key_s].down || in.keys[key_down].down ? 1.0f : 0.0f);
        fx = (in.keys[key_d].down || in.keys[key_right].down ? 1.0f : 0.0f) -
             (in.keys[key_a].down || in.keys[key_left].down ? 1.0f : 0.0f);
    }
    // Relative to the camera: forward is where it looks.
    const V3 forward = {-std::sin(hub.cam_yaw), 0, -std::cos(hub.cam_yaw)};
    const V3 right = {std::cos(hub.cam_yaw), 0, -std::sin(hub.cam_yaw)};
    V3 move = forward * fz + right * fx;
    const float amount = length(move);
    const bool run = in.keys[key_shift].down;
    if (amount > 0) {
        move = move * (1 / amount);
        const float speed = run ? run_speed : walk_speed;
        me.x += move.x * speed * in.dt;
        me.z += move.z * speed * in.dt;
        me.yaw = lerp_angle(me.yaw, std::atan2(-move.x, -move.z), approach(14, in.dt));
        me.anim = run ? 2 : 1;
        me.walk += in.dt * (run ? 13.0f : 9.0f);
    } else {
        me.anim = 0;
    }
    collide_world(me.x, me.z);
    // A hop.
    if (!hub.typing && in.keys[key_space].pressed && me.y <= 0) {
        hub.vy = 4.2f;
    }
    hub.vy -= 13 * in.dt;
    me.y = std::max(0.0f, me.y + hub.vy * in.dt);
    if (me.y <= 0) {
        hub.vy = 0;
    }
}

// --- Bowling, as the one playing.

static V3 lane_point(int index, float x, float y, float d) {
    return {layout_lane_x[index] + x, y, layout_foul_z - d};
}

static void bowl(Hub& hub, const Input& in, double now) {
    Lane& lane = hub.lanes[hub.my_lane];
    Player& me = hub.me;
    if (in.keys[key_escape].pressed) {
        lane = {};
        hub.my_lane = -1;
        return;
    }
    if (lane.state == LaneState::Over && in.keys[key_space].pressed) {
        start_game(lane);
    } else if (lane.state == LaneState::Aiming) {
        const float aim_speed = 0.6f * in.dt;
        if (in.keys[key_a].down || in.keys[key_left].down) {
            lane.aim_x -= aim_speed;
        }
        if (in.keys[key_d].down || in.keys[key_right].down) {
            lane.aim_x += aim_speed;
        }
        if (in.keys[key_q].down) {
            lane.aim_angle -= 0.05f * in.dt;
        }
        if (in.keys[key_e].down) {
            lane.aim_angle += 0.05f * in.dt;
        }
        lane.aim_x = clampf(lane.aim_x, -layout_lane_half + ball_radius, layout_lane_half - ball_radius);
        lane.aim_angle = clampf(lane.aim_angle, -0.035f, 0.035f);
        lane.ball.x = lane.aim_x;
        // Holding Space: the power swings up and down; letting go throws.
        if (in.keys[key_space].pressed) {
            lane.charging = true;
            lane.charge = 0;
        }
        if (lane.charging) {
            lane.charge += in.dt;
            lane.power = 0.5f - 0.5f * std::cos(lane.charge * 2.6f);
            if (in.keys[key_space].released) {
                throw_ball(lane);
            }
        }
    }
    step_lane(lane, in.dt, now);
    // We stand by the ball (to its left, out of the camera's way), facing the pins.
    me.x = layout_lane_x[hub.my_lane] + lane.aim_x - 0.42f;
    me.z = layout_foul_z + 0.75f;
    me.yaw = 0;
    me.anim = 0;
    if (now - hub.lane_sent_at >= lane_send_interval) {
        platform_send("bowl " + std::to_string(hub.my_lane) + " " + encode_lane(lane));
        hub.lane_sent_at = now;
    }
}

// --- Drawing.

static void draw_crewmate(Hub& hub, const Player& p, double now) {
    // A walk is a little bounce and sway.
    const float moving = p.anim > 0 ? 1.0f : 0.0f;
    const float bounce = std::abs(std::sin(p.walk)) * 0.07f * moving;
    const float sway = std::sin(p.walk) * 0.08f * moving;
    const M4 model = translation({p.x, p.y + bounce, p.z}) * rotation_y(p.yaw) * rotation({0, 0, 1}, sway);
    draw_mesh(hub.r, hub.crewmate, model, p.color);
    (void)now;
}

static void draw_lane(Hub& hub, int index, const Lane& lane) {
    if (lane.state == LaneState::Free) {
        // Racked, waiting for a player.
        Lane racked;
        reset_pins(racked);
        for (const Pin& p : racked.pins) {
            draw_mesh(hub.r, hub.pin, translation(lane_point(index, p.x, lane_surface, p.d)));
        }
        return;
    }
    for (const Pin& p : lane.pins) {
        if (p.gone) {
            continue;
        }
        // Falling over its foot, away from what hit it.
        const V3 base = lane_point(index, p.x, lane_surface, p.d);
        // (Turning the up of the pin toward where it falls, (fall_x, 0, -fall_d): around up x that.)
        const V3 axis = normalize({-p.fall_d, 0, -p.fall_x});
        M4 model = translation(base);
        if (p.tilt > 0 && length(axis) > 0) {
            model = model * rotation(axis, p.tilt * pi / 2 * 0.97f);
        }
        draw_mesh(hub.r, hub.pin, model);
    }
    const Ball& b = lane.ball;
    if (b.rolling || lane.state == LaneState::Aiming) {
        const float y = b.gutter ? lane_surface - 0.04f + ball_radius : lane_surface + ball_radius;
        const M4 model = translation(lane_point(index, b.x, y, b.d)) * rotation({1, 0, 0}, -b.roll);
        const Player* owner = lane.mine ? &hub.me : other(hub, lane.owner);
        draw_mesh(hub.r, hub.ball, model, owner ? owner->color : V3 {0.3f, 0.4f, 0.9f});
    }
}

static void draw_world(Hub& hub, const Input& in, double now) {
    const float aspect = static_cast<float>(in.width) / static_cast<float>(std::max(1, in.height));
    V3 target = {hub.me.x, hub.me.y + 1.1f, hub.me.z};
    V3 eye;
    if (hub.my_lane >= 0) {
        // Bowling: behind the ball, following it down the lane.
        const Lane& lane = hub.lanes[hub.my_lane];
        const float follow = lane.state == LaneState::Rolling ? clampf(lane.ball.d - 2.5f, 0, layout_headpin - 5) : 0;
        // (High and far enough to see the ball at our feet and the pins at the end.)
        eye = lane_point(hub.my_lane, lane.aim_x * 0.5f, 2.25f, follow - 3.4f);
        target = lane_point(hub.my_lane, lane.state == LaneState::Rolling ? lane.ball.x : lane.aim_x, 0.2f,
                            follow + 3.5f);
    } else {
        const V3 dir = {-std::sin(hub.cam_yaw) * std::cos(hub.cam_pitch), -std::sin(hub.cam_pitch),
                        -std::cos(hub.cam_yaw) * std::cos(hub.cam_pitch)};
        eye = target - dir * hub.cam_distance;
        eye.y = std::max(eye.y, 0.4f);
    }
    hub.cam_pos = lerp(hub.cam_pos, eye, hub.frame < 2 ? 1.0f : approach(hub.my_lane >= 0 ? 6.0f : 18.0f, in.dt));
    const M4 view = look_at(hub.cam_pos, target, {0, 1, 0});
    const M4 projection = perspective(55 * pi / 180, aspect, 0.1f, 200);
    begin_frame(hub.r, in.width, in.height, projection * view, hub.cam_pos);

    draw_mesh(hub.r, hub.plaza, M4 {});
    for (int i = 0; i < 2; ++i) {
        draw_lane(hub, i, hub.lanes[i]);
    }
    draw_crewmate(hub, hub.me, now);
    for (const Player& p : hub.others) {
        draw_crewmate(hub, p, now);
    }
    begin_transparent();
    const auto shadow = [&](const Player& p) {
        const float fade = clampf(1 - p.y / 2.5f, 0.2f, 1);
        draw_mesh(hub.r, hub.shadow, translation({p.x, 0.04f, p.z}) * scaling(0.6f + 0.4f * fade), {1, 1, 1},
                  0.32f * fade);
    };
    shadow(hub.me);
    for (const Player& p : hub.others) {
        shadow(p);
    }
    end_transparent();
}

static void draw_labels(Hub& hub, const Input& in, double now) {
    const auto label = [&](const Player& p) {
        V2 at;
        if (!to_screen(hub.r, {p.x, p.y + 1.55f, p.z}, in.width, in.height, at)) {
            return;
        }
        const float dist = length(V3 {p.x, p.y, p.z} - hub.cam_pos);
        const float fade = clampf(1.4f - dist / 30, 0, 1);
        if (fade <= 0) {
            return;
        }
        const V4 c = {p.color.x * 0.4f + 0.6f, p.color.y * 0.4f + 0.6f, p.color.z * 0.4f + 0.6f, fade};
        draw_text(hub, p.name.empty() ? std::string("...") : p.name, at.x, at.y - 22, 17, c, 1, true, true);
        if (!p.bubble.empty() && now - p.bubble_at < bubble_life) {
            const float life = static_cast<float>(now - p.bubble_at);
            const float alpha = fade * clampf((bubble_life - life) / 1.0f, 0, 1);
            draw_text(hub, p.bubble, at.x, at.y - 56, 19, {1, 1, 1, alpha}, 1, false, true);
        }
    };
    for (const Player& p : hub.others) {
        label(p);
    }
    // (Not over our own head while bowling: the camera is right behind it.)
    if (hub.my_lane < 0) {
        label(hub.me);
    }
    // Over each lane being played: who, and their score.
    for (int i = 0; i < 2; ++i) {
        const Lane& lane = hub.lanes[i];
        if (lane.state == LaneState::Free || (hub.my_lane == i)) {
            continue;
        }
        V2 at;
        if (to_screen(hub.r, lane_point(i, 0, 2.3f, layout_headpin - 0.6f), in.width, in.height, at)) {
            draw_text(hub, lane.owner_name + " is bowling", at.x, at.y - 40, 16, {1, 0.9f, 0.6f, 1}, 1, true, true);
            draw_text(hub, scoreboard(lane), at.x, at.y - 14, 15, {1, 1, 1, 1}, 1, false, true);
        }
    }
}

static void draw_hud(Hub& hub, const Input& in, double now) {
    const float w = static_cast<float>(in.width), h = static_cast<float>(in.height);
    const V4 white = {1, 1, 1, 1}, soft = {1, 1, 1, 0.75f};
    draw_text(hub, std::string("zchat hub") + (platform_connected() ? "" : "  (offline: start it with /hub in zchat)"),
              14, 12, 18, white, 0, true, true);
    if (hub.my_lane >= 0) {
        const Lane& lane = hub.lanes[hub.my_lane];
        draw_text(hub, scoreboard(lane), w / 2, 14, 20, white, 1, true, true);
        if (!lane.news.empty() && now - lane.news_at < 2.2) {
            const float a = clampf(static_cast<float>(2.2 - (now - lane.news_at)), 0, 1);
            draw_text(hub, lane.news, w / 2, h * 0.32f, 46, {1, 0.85f, 0.3f, a}, 1, true);
        }
        if (lane.state == LaneState::Aiming) {
            draw_text(hub, "A/D move    Q/E angle    hold Space for power, let go to throw    Esc leave", w / 2,
                      h - 44, 17, soft, 1, false, true);
            // The power meter.
            const float mw = 260, mx = w / 2 - mw / 2, my = h - 78;
            draw_rect(hub.r, mx - 3, my - 3, mw + 6, 18, {0, 0, 0, 0.5f});
            draw_rect(hub.r, mx, my, mw * lane.power, 12,
                      {0.3f + 0.7f * lane.power, 0.85f - 0.5f * lane.power, 0.3f, 1});
        } else if (lane.state == LaneState::Over) {
            draw_text(hub, "Space plays again    Esc leaves the lane", w / 2, h - 44, 17, soft, 1, false, true);
        }
        return;
    }
    if (hub.typing) {
        const float bw = std::min(w - 28, 720.0f);
        draw_rect(hub.r, w / 2 - bw / 2, h - 58, bw, 40, {0.08f, 0.09f, 0.12f, 0.8f});
        const bool blink = std::fmod(now, 1.0) < 0.55;
        draw_text(hub, "Say: " + hub.draft + (blink ? "|" : " "), w / 2 - bw / 2 + 12, h - 50, 19, white);
        return;
    }
    const int lane = lane_nearby(hub);
    if (lane >= 0) {
        const std::string what = lane_taken(hub, lane) ? "Lane " + std::to_string(lane + 1) + " is taken"
                                                       : "E  Bowl on lane " + std::to_string(lane + 1);
        draw_text(hub, what, w / 2, h * 0.62f, 22, {1, 0.9f, 0.55f, 1}, 1, true, true);
    }
    draw_text(hub, "WASD walk    Shift run    Space hop    drag to look    wheel zoom    Enter talk", w / 2, h - 40,
              16, soft, 1, false, true);
}

// --- A frame.

static void start(Hub& hub) {
    renderer_init(hub.r);
    hub.plaza = load_mesh(plaza_mesh, plaza_mesh_size);
    hub.crewmate = load_mesh(crewmate_mesh, crewmate_mesh_size);
    hub.pin = load_mesh(pin_mesh, pin_mesh_size);
    hub.ball = load_mesh(ball_mesh, ball_mesh_size);
    hub.shadow = load_mesh(shadow_mesh, shadow_mesh_size);
    hub.me.name = "You";
    hub.me.color = {0.95f, 0.45f, 0.35f};
    hub.me.x = layout_spawn[0] + (static_cast<float>(std::rand() % 100) - 50) / 40;
    hub.me.z = layout_spawn[1] + static_cast<float>(std::rand() % 100) / 50;
    hub.ready = true;
}

void hub_frame(Hub& hub, const Input& in) {
    if (!hub.ready) {
        start(hub);
    }
    ++hub.frame;
    const double now = in.time;
    for (std::string line; platform_receive(line);) {
        receive(hub, line, now);
    }

    // Typing: Enter starts and sends, Escape gives up.
    if (hub.my_lane < 0) {
        if (!hub.typing && in.keys[key_enter].pressed) {
            hub.typing = true;
            hub.draft.clear();
        } else if (hub.typing) {
            hub.draft += in.typed;
            if (in.keys[key_backspace].pressed && !hub.draft.empty()) {
                // A whole character, not a byte of it.
                std::size_t cut = hub.draft.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(hub.draft[cut]) & 0xC0) == 0x80) {
                    --cut;
                }
                hub.draft.erase(cut);
            }
            if (in.keys[key_escape].pressed) {
                hub.typing = false;
            } else if (in.keys[key_enter].pressed) {
                hub.typing = false;
                if (!hub.draft.empty()) {
                    platform_send("say " + hub.draft);
                    if (!platform_connected()) {
                        hub.me.bubble = hub.draft;
                        hub.me.bubble_at = now;
                    }
                }
            }
        }
    }

    // The camera: dragged to look around, the wheel to come closer.
    if (in.mouse_left.down || in.mouse_right.down) {
        hub.cam_yaw -= in.mouse_dx * 0.006f;
        hub.cam_pitch = clampf(hub.cam_pitch + in.mouse_dy * 0.005f, -0.15f, 1.25f);
    }
    hub.cam_distance = clampf(hub.cam_distance * std::pow(0.88f, in.wheel), 2.5f, 16.0f);

    if (hub.my_lane >= 0) {
        bowl(hub, in, now);
    } else {
        walk(hub, in);
        const int lane = lane_nearby(hub);
        if (!hub.typing && lane >= 0 && in.keys[key_e].pressed && !lane_taken(hub, lane)) {
            hub.my_lane = lane;
            hub.lanes[lane] = {};
            hub.lanes[lane].mine = true;
            hub.lanes[lane].owner = hub.me.id;
            hub.lanes[lane].owner_name = hub.me.name;
            start_game(hub.lanes[lane]);
        }
    }
    send_place(hub, now);

    // The others: toward where they said they are; gone when silent; lanes nobody plays any more.
    for (Player& p : hub.others) {
        const float t = approach(10, in.dt);
        const float dx = p.tx - p.x, dz = p.tz - p.z;
        p.x += dx * t;
        p.z += dz * t;
        p.yaw = lerp_angle(p.yaw, p.tyaw, approach(12, in.dt));
        if (p.anim > 0) {
            p.walk += in.dt * (p.anim == 2 ? 13.0f : 9.0f);
        }
    }
    std::erase_if(hub.others, [&](const Player& p) {
        return now - p.heard > timeout;
    });
    for (Lane& lane : hub.lanes) {
        if (!lane.mine && lane.state != LaneState::Free && now - lane.heard > 4) {
            lane = {};
        }
    }

    draw_world(hub, in, now);
    begin_overlay(hub.r, in.width, in.height);
    draw_labels(hub, in, now);
    draw_hud(hub, in, now);
}

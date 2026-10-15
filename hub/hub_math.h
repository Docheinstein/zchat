// The math of the hub, by hand: vectors, and 4x4 matrices stored by columns, as OpenGL takes them.
#pragma once

#include <cmath>

constexpr float pi = 3.14159265358979f;

struct V2 {
    float x = 0, y = 0;
};

struct V3 {
    float x = 0, y = 0, z = 0;
};

struct V4 {
    float x = 0, y = 0, z = 0, w = 0;
};

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(float s, V3 a) { return a * s; }
inline V3 hadamard(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) {
    const float l = length(a);
    return l > 0 ? a * (1.0f / l) : V3 {};
}
inline V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
// From a toward b, an angle, the short way round.
inline float lerp_angle(float a, float b, float t) {
    float d = std::fmod(b - a + 3 * pi, 2 * pi) - pi;
    return a + d * t;
}
// What a smoothing of rate per second leaves of the way after dt: lerp(a, b, approach(rate, dt)).
inline float approach(float rate, float dt) { return 1.0f - std::exp(-rate * dt); }

struct M4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline M4 operator*(const M4& a, const M4& b) {
    M4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

inline V4 transform(const M4& a, V4 v) {
    return {a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w,
            a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w,
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w,
            a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w};
}

inline M4 translation(V3 t) {
    M4 r;
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

inline M4 scaling(float s) {
    M4 r;
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}

// Around the vertical: angle 0 faces -z, as the models do.
inline M4 rotation_y(float a) {
    M4 r;
    const float c = std::cos(a), s = std::sin(a);
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}

// Around any axis (normalized), right-handed.
inline M4 rotation(V3 axis, float a) {
    const float c = std::cos(a), s = std::sin(a), t = 1 - c;
    const float x = axis.x, y = axis.y, z = axis.z;
    M4 r;
    r.m[0] = t * x * x + c;
    r.m[1] = t * x * y + s * z;
    r.m[2] = t * x * z - s * y;
    r.m[4] = t * x * y - s * z;
    r.m[5] = t * y * y + c;
    r.m[6] = t * y * z + s * x;
    r.m[8] = t * x * z + s * y;
    r.m[9] = t * y * z - s * x;
    r.m[10] = t * z * z + c;
    return r;
}

inline M4 perspective(float fov_y, float aspect, float near_plane, float far_plane) {
    const float f = 1.0f / std::tan(fov_y / 2);
    M4 r;
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    r.m[11] = -1;
    r.m[14] = 2 * far_plane * near_plane / (near_plane - far_plane);
    r.m[15] = 0;
    return r;
}

inline M4 look_at(V3 eye, V3 target, V3 up) {
    const V3 f = normalize(target - eye);
    const V3 s = normalize(cross(f, up));
    const V3 u = cross(s, f);
    M4 r;
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

// Pixels from the top left to OpenGL's clip space.
inline M4 pixels(float width, float height) {
    M4 r;
    r.m[0] = 2 / width;
    r.m[5] = -2 / height;
    r.m[12] = -1;
    r.m[13] = 1;
    return r;
}

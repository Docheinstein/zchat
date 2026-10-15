// The renderer: meshes lit by a sun and the sky, fading into the fog far away, and textured quads in pixels for the
// text and the HUD. Two shaders, one draw call per mesh: the hub is small.

#include "hub_gl.h"
#include "hub_math.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

struct Mesh {
    GLuint vao = 0;
    GLsizei count = 0;
};

struct Texture {
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

struct Renderer {
    GLuint mesh_program = 0;
    GLint u_mvp = -1, u_model = -1, u_tint = -1, u_alpha = -1, u_camera = -1, u_fog = -1;
    GLuint quad_program = 0;
    GLint u_quad_projection = -1, u_quad_color = -1, u_quad_texture = -1;
    GLuint quad_vao = 0, quad_vbo = 0;
    Texture white;
    M4 view_projection;
    V3 camera;
};

static const V3 fog_color = {0.70f, 0.83f, 0.95f};

static const char* mesh_vertex_shader = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_color;
uniform mat4 u_mvp;
uniform mat4 u_model;
out vec3 v_normal;
out vec4 v_color;
out vec3 v_world;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
    v_world = (u_model * vec4(a_position, 1.0)).xyz;
    v_normal = mat3(u_model) * a_normal;
    v_color = a_color;
}
)";

static const char* mesh_fragment_shader = R"(#version 330 core
in vec3 v_normal;
in vec4 v_color;
in vec3 v_world;
uniform vec3 u_tint;
uniform float u_alpha;
uniform vec3 u_camera;
uniform vec3 u_fog;
out vec4 o_color;
void main() {
    // The color's alpha says whether the player's color tints it.
    vec3 base = mix(v_color.rgb, v_color.rgb * u_tint, v_color.a);
    vec3 n = normalize(v_normal);
    vec3 sun = normalize(vec3(0.45, 0.85, 0.30));
    float lit = max(dot(n, sun), 0.0);
    vec3 ambient = mix(vec3(0.46, 0.42, 0.36), vec3(0.66, 0.76, 0.90), n.y * 0.5 + 0.5);
    vec3 c = base * (ambient * 0.62 + vec3(1.0, 0.95, 0.86) * lit * 0.72);
    float far = smoothstep(32.0, 85.0, length(v_world - u_camera));
    o_color = vec4(mix(c, u_fog, far), u_alpha);
}
)";

static const char* quad_vertex_shader = R"(#version 330 core
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_projection;
out vec2 v_uv;
void main() {
    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);
    v_uv = a_uv;
}
)";

static const char* quad_fragment_shader = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_texture;
uniform vec4 u_color;
out vec4 o_color;
void main() {
    o_color = texture(u_texture, v_uv) * u_color;
}
)";

static GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "hub: a shader does not compile: %s\n", log);
    }
    return shader;
}

static GLuint link(const char* vertex, const char* fragment) {
    const GLuint program = glCreateProgram();
    const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::fprintf(stderr, "hub: the shaders do not link: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

static Texture make_texture(int width, int height, const std::uint8_t* rgba) {
    Texture t;
    t.width = width;
    t.height = height;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void free_texture(Texture& t) {
    if (t.id) {
        glDeleteTextures(1, &t.id);
    }
    t = {};
}

static void renderer_init(Renderer& r) {
    r.mesh_program = link(mesh_vertex_shader, mesh_fragment_shader);
    r.u_mvp = glGetUniformLocation(r.mesh_program, "u_mvp");
    r.u_model = glGetUniformLocation(r.mesh_program, "u_model");
    r.u_tint = glGetUniformLocation(r.mesh_program, "u_tint");
    r.u_alpha = glGetUniformLocation(r.mesh_program, "u_alpha");
    r.u_camera = glGetUniformLocation(r.mesh_program, "u_camera");
    r.u_fog = glGetUniformLocation(r.mesh_program, "u_fog");

    r.quad_program = link(quad_vertex_shader, quad_fragment_shader);
    r.u_quad_projection = glGetUniformLocation(r.quad_program, "u_projection");
    r.u_quad_color = glGetUniformLocation(r.quad_program, "u_color");
    r.u_quad_texture = glGetUniformLocation(r.quad_program, "u_texture");
    glGenVertexArrays(1, &r.quad_vao);
    glBindVertexArray(r.quad_vao);
    glGenBuffers(1, &r.quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r.quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    const std::uint8_t white[4] = {255, 255, 255, 255};
    r.white = make_texture(1, 1, white);
}

// A mesh made by hub/assets/make_assets.py ("HUBM", the counts, the vertices, the indices).
static Mesh load_mesh(const unsigned char* data, std::size_t size) {
    Mesh mesh;
    std::uint32_t vertices = 0, indices = 0;
    if (size < 12 || std::memcmp(data, "HUBM", 4) != 0) {
        return mesh;
    }
    std::memcpy(&vertices, data + 4, 4);
    std::memcpy(&indices, data + 8, 4);
    const std::size_t vertex_bytes = static_cast<std::size_t>(vertices) * 28;
    const std::size_t index_bytes = static_cast<std::size_t>(indices) * 4;
    if (12 + vertex_bytes + index_bytes > size) {
        return mesh;
    }
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);
    GLuint buffers[2];
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertex_bytes), data + 12, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(index_bytes), data + 12 + vertex_bytes,
                 GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 28, reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 28, reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(2);
    mesh.count = static_cast<GLsizei>(indices);
    return mesh;
}

// Starts a frame: the sky, and where the camera is.
static void begin_frame(Renderer& r, int width, int height, const M4& view_projection, V3 camera) {
    glViewport(0, 0, width, height);
    glClearColor(fog_color.x, fog_color.y, fog_color.z, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    r.view_projection = view_projection;
    r.camera = camera;
    glUseProgram(r.mesh_program);
    glUniform3f(r.u_camera, camera.x, camera.y, camera.z);
    glUniform3f(r.u_fog, fog_color.x, fog_color.y, fog_color.z);
}

static void draw_mesh(Renderer& r, const Mesh& mesh, const M4& model, V3 tint = {1, 1, 1}, float alpha = 1) {
    if (!mesh.vao) {
        return;
    }
    const M4 mvp = r.view_projection * model;
    glUniformMatrix4fv(r.u_mvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(r.u_model, 1, GL_FALSE, model.m);
    glUniform3f(r.u_tint, tint.x, tint.y, tint.z);
    glUniform1f(r.u_alpha, alpha);
    glBindVertexArray(mesh.vao);
    glDrawElements(GL_TRIANGLES, mesh.count, GL_UNSIGNED_INT, nullptr);
}

// See-through things (the shadows): after the solid ones, without writing the depth.
static void begin_transparent() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1, -1);
}

static void end_transparent() {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

// From here on, flat things in pixels over the scene.
static void begin_overlay(Renderer& r, int width, int height) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(r.quad_program);
    const M4 projection = pixels(static_cast<float>(width), static_cast<float>(height));
    glUniformMatrix4fv(r.u_quad_projection, 1, GL_FALSE, projection.m);
    glUniform1i(r.u_quad_texture, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(r.quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r.quad_vbo);
}

static void draw_quad(Renderer& r, float x, float y, float w, float h, const Texture& texture, V4 color) {
    const float v[24] = {x,     y, 0, 0, x + w, y, 1, 0, x + w, y + h, 1, 1,
                         x,     y, 0, 0, x + w, y + h, 1, 1, x, y + h, 0, 1};
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glUniform4f(r.u_quad_color, color.x, color.y, color.z, color.w);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void draw_rect(Renderer& r, float x, float y, float w, float h, V4 color) {
    draw_quad(r, x, y, w, h, r.white, color);
}

// Where a point of the world is on the screen, in pixels; false when it is behind the camera.
static bool to_screen(const Renderer& r, V3 p, int width, int height, V2& out) {
    const V4 clip = transform(r.view_projection, {p.x, p.y, p.z, 1});
    if (clip.w <= 0.05f) {
        return false;
    }
    out.x = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(width);
    out.y = (1 - (clip.y / clip.w * 0.5f + 0.5f)) * static_cast<float>(height);
    return true;
}

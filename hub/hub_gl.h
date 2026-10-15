// OpenGL 3.3 (core), without a loader library: the system's header has OpenGL 1.1, and what is newer is listed here
// and looked up by the platform when the context is made (see load_gl() in win32_hub.cpp).
#pragma once

#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#ifndef APIENTRY
#define APIENTRY
#endif

using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MULTISAMPLE 0x809D

// What is looked up: return type, name, parameters.
#define HUB_GL_FUNCTIONS(X)                                                                                            \
    X(GLuint, glCreateShader, (GLenum type))                                                                           \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))         \
    X(void, glCompileShader, (GLuint shader))                                                                          \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint* params))                                               \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei* length, GLchar* log))                           \
    X(void, glDeleteShader, (GLuint shader))                                                                           \
    X(GLuint, glCreateProgram, (void))                                                                                 \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                                           \
    X(void, glLinkProgram, (GLuint program))                                                                           \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint* params))                                             \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei size, GLsizei* length, GLchar* log))                         \
    X(void, glUseProgram, (GLuint program))                                                                            \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))                                               \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value))            \
    X(void, glUniform4f, (GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w))                                 \
    X(void, glUniform3f, (GLint location, GLfloat x, GLfloat y, GLfloat z))                                            \
    X(void, glUniform1f, (GLint location, GLfloat x))                                                                  \
    X(void, glUniform1i, (GLint location, GLint x))                                                                    \
    X(void, glGenVertexArrays, (GLsizei n, GLuint * arrays))                                                           \
    X(void, glBindVertexArray, (GLuint array))                                                                         \
    X(void, glGenBuffers, (GLsizei n, GLuint * buffers))                                                               \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                                              \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                            \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))                      \
    X(void, glVertexAttribPointer,                                                                                     \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer))              \
    X(void, glEnableVertexAttribArray, (GLuint index))                                                                 \
    X(void, glActiveTexture, (GLenum texture))

#define HUB_GL_DECLARE(ret, name, params) static ret(APIENTRY* name) params = nullptr;
HUB_GL_FUNCTIONS(HUB_GL_DECLARE)
#undef HUB_GL_DECLARE

#pragma once
// Minimal OpenGL 3.3 core loader via SDL_GL_GetProcAddress (no external deps).
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstdio>

namespace ht2 {
struct GL {
    bool ok = false;

    PFNGLCREATESHADERPROC         glCreateShader = nullptr;
    PFNGLSHADERSOURCEPROC         glShaderSource = nullptr;
    PFNGLCOMPILESHADERPROC        glCompileShader = nullptr;
    PFNGLGETSHADERIVPROC          glGetShaderiv = nullptr;
    PFNGLGETSHADERINFOLOGPROC     glGetShaderInfoLog = nullptr;
    PFNGLCREATEPROGRAMPROC        glCreateProgram = nullptr;
    PFNGLATTACHSHADERPROC         glAttachShader = nullptr;
    PFNGLLINKPROGRAMPROC          glLinkProgram = nullptr;
    PFNGLGETPROGRAMIVPROC         glGetProgramiv = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC    glGetProgramInfoLog = nullptr;
    PFNGLUSEPROGRAMPROC           glUseProgram = nullptr;
    PFNGLDELETEPROGRAMPROC        glDeleteProgram = nullptr;
    PFNGLDELETEBUFFERSPROC        glDeleteBuffers = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC   glDeleteVertexArrays = nullptr;
    PFNGLGENVERTEXARRAYSPROC      glGenVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC      glBindVertexArray = nullptr;
    PFNGLGENBUFFERSPROC           glGenBuffers = nullptr;
    PFNGLBINDBUFFERPROC           glBindBuffer = nullptr;
    PFNGLBUFFERDATAPROC           glBufferData = nullptr;
    PFNGLVERTEXATTRIBPOINTERPROC  glVertexAttribPointer = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC   glGetUniformLocation = nullptr;
    PFNGLUNIFORMMATRIX4FVPROC     glUniformMatrix4fv = nullptr;
    PFNGLUNIFORM1IPROC            glUniform1i = nullptr;
    PFNGLUNIFORM1FPROC            glUniform1f = nullptr;
    void (*glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    PFNGLUNIFORM3FPROC            glUniform3f = nullptr;
    // GL 1.1/1.3 entry points: the typedefs are not always visible when SDL
    // includes the core-profile headers, so declare the signatures directly.
    void (*glGenTextures)(GLsizei, GLuint*) = nullptr;
    void (*glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void (*glBindTexture)(GLenum, GLuint) = nullptr;
    void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                         GLenum, GLenum, const void*) = nullptr;
    void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void (*glActiveTexture)(GLenum) = nullptr;
    void (*glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
    void (*glGenerateMipmap)(GLenum) = nullptr;

    bool load() {
        #define HT2_GL(FN) FN = (decltype(FN))SDL_GL_GetProcAddress(#FN); if(!FN) { std::fprintf(stderr, "[ht2re] missing GL proc: %s\n", #FN); ok=false; return false; }
        HT2_GL(glCreateShader)
        HT2_GL(glShaderSource)
        HT2_GL(glCompileShader)
        HT2_GL(glGetShaderiv)
        HT2_GL(glGetShaderInfoLog)
        HT2_GL(glCreateProgram)
        HT2_GL(glAttachShader)
        HT2_GL(glLinkProgram)
        HT2_GL(glGetProgramiv)
        HT2_GL(glGetProgramInfoLog)
        HT2_GL(glUseProgram)
        HT2_GL(glDeleteProgram)
        HT2_GL(glDeleteBuffers)
        HT2_GL(glDeleteVertexArrays)
        HT2_GL(glGenVertexArrays)
        HT2_GL(glBindVertexArray)
        HT2_GL(glGenBuffers)
        HT2_GL(glBindBuffer)
        HT2_GL(glBufferData)
        HT2_GL(glVertexAttribPointer)
        HT2_GL(glEnableVertexAttribArray)
        HT2_GL(glGetUniformLocation)
        HT2_GL(glUniformMatrix4fv)
        HT2_GL(glUniform1i)
        HT2_GL(glUniform1f)
        HT2_GL(glUniform2f)
        HT2_GL(glUniform3f)
        HT2_GL(glGenTextures)
        HT2_GL(glDeleteTextures)
        HT2_GL(glBindTexture)
        HT2_GL(glTexImage2D)
        HT2_GL(glTexParameteri)
        HT2_GL(glActiveTexture)
        HT2_GL(glDrawArrays)
        HT2_GL(glGenerateMipmap)
        #undef HT2_GL
        ok = true;
        return true;
    }
};

} // namespace ht2
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The OpenGL ES 3.2 entry points the GLES renderer calls (docs/RG35XXH.md), loaded at run time through the window
// system's loader (SDL_GL_GetProcAddress): the executable links no GL library, so one build runs on any driver
// (Mali's libmali on KNULLI, Mesa on a desktop). The pointers live in galaxy::gx::gles::gl under the GL names, so a
// file that says `using namespace galaxy::gx::gles::gl;` calls glDrawElements(...) as usual while no global symbol
// can interpose on the driver's own.

#ifndef GL_GLES_PROTOTYPES
#define GL_GLES_PROTOTYPES 0
#endif
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <string>

// X(type, name): every entry point; the core ones (GLES_CORE) must load, the others (GLES_OPTIONAL) may be null.
#define GLES_CORE_FUNCTIONS(X)                                                    \
    X(PFNGLGETSTRINGPROC, glGetString)                                            \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                          \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                        \
    X(PFNGLGETINTEGER64VPROC, glGetInteger64v)                                    \
    X(PFNGLGETFLOATVPROC, glGetFloatv)                                            \
    X(PFNGLGETERRORPROC, glGetError)                                              \
    X(PFNGLENABLEPROC, glEnable)                                                  \
    X(PFNGLDISABLEPROC, glDisable)                                                \
    X(PFNGLVIEWPORTPROC, glViewport)                                              \
    X(PFNGLSCISSORPROC, glScissor)                                                \
    X(PFNGLDEPTHRANGEFPROC, glDepthRangef)                                        \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                          \
    X(PFNGLCLEARDEPTHFPROC, glClearDepthf)                                        \
    X(PFNGLCLEARPROC, glClear)                                                    \
    X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv)                                    \
    X(PFNGLCLEARBUFFERFIPROC, glClearBufferfi)                                    \
    X(PFNGLCOLORMASKPROC, glColorMask)                                            \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                            \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                            \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                            \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                    \
    X(PFNGLCULLFACEPROC, glCullFace)                                              \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                            \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                        \
    X(PFNGLFINISHPROC, glFinish)                                                  \
    X(PFNGLFLUSHPROC, glFlush)                                                    \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                      \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                      \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                    \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                        \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                              \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                      \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                    \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                      \
    X(PFNGLDETACHSHADERPROC, glDetachShader)                                      \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                        \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                      \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                            \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                          \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                    \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                          \
    X(PFNGLGETACTIVEATTRIBPROC, glGetActiveAttrib)                                \
    X(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation)                            \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                            \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex)                      \
    X(PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding)                        \
    X(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary)                              \
    X(PFNGLPROGRAMBINARYPROC, glProgramBinary)                                    \
    X(PFNGLPROGRAMPARAMETERIPROC, glProgramParameteri)                            \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                          \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                    \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                          \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                          \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                    \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                                \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                                  \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange)                                  \
    X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer)                                        \
    X(PFNGLFLUSHMAPPEDBUFFERRANGEPROC, glFlushMappedBufferRange)                  \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                          \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)              \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                        \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer)                      \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                          \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                                      \
    X(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced)                        \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                        \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                  \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                        \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                    \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                          \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)                                    \
    X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D)                                      \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                    \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers)                                        \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                                  \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                        \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                            \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf)                            \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                          \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                      \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                  \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)                                \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                          \
    X(PFNGLREADBUFFERPROC, glReadBuffer)                                          \
    X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)                                        \
    X(PFNGLINVALIDATEFRAMEBUFFERPROC, glInvalidateFramebuffer)                    \
    X(PFNGLFENCESYNCPROC, glFenceSync)                                            \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync)                                  \
    X(PFNGLDELETESYNCPROC, glDeleteSync)                                          \
    X(PFNGLGETSYNCIVPROC, glGetSynciv)

#define GLES_OPTIONAL_FUNCTIONS(X)                                                \
    X(PFNGLVERTEXATTRIBFORMATPROC, glVertexAttribFormat)                          \
    X(PFNGLVERTEXATTRIBIFORMATPROC, glVertexAttribIFormat)                        \
    X(PFNGLVERTEXATTRIBBINDINGPROC, glVertexAttribBinding)                        \
    X(PFNGLBINDVERTEXBUFFERPROC, glBindVertexBuffer)                              \
    X(PFNGLTEXBUFFERRANGEPROC, glTexBufferRange)                                  \
    X(PFNGLTEXBUFFERRANGEEXTPROC, glTexBufferRangeEXT)                            \
    X(PFNGLBUFFERSTORAGEEXTPROC, glBufferStorageEXT)                              \
    X(PFNGLGENQUERIESPROC, glGenQueries)                                          \
    X(PFNGLDELETEQUERIESPROC, glDeleteQueries)                                    \
    X(PFNGLBEGINQUERYPROC, glBeginQuery)                                          \
    X(PFNGLENDQUERYPROC, glEndQuery)                                              \
    X(PFNGLQUERYCOUNTEREXTPROC, glQueryCounterEXT)                                \
    X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv)                            \
    X(PFNGLGETQUERYOBJECTUI64VEXTPROC, glGetQueryObjectui64vEXT)                  \
    X(PFNGLDEBUGMESSAGECALLBACKKHRPROC, glDebugMessageCallbackKHR)                \
    X(PFNGLGETPROGRAMRESOURCEINDEXPROC, glGetProgramResourceIndex)

namespace galaxy::gx::gles::gl {

#define GLES_DECLARE(type, name) extern type name;
GLES_CORE_FUNCTIONS(GLES_DECLARE)
GLES_OPTIONAL_FUNCTIONS(GLES_DECLARE)
#undef GLES_DECLARE

using Loader = void* (*)(const char* name);
// Loads every entry point through `loader` (the context must be current); false naming the first core one missing.
bool load(Loader loader, std::string& missing);
// The driver's extension list has `name` (after load).
[[nodiscard]] bool has_extension(const char* name);

}  // namespace galaxy::gx::gles::gl

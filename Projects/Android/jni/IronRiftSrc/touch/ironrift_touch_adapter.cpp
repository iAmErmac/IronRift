#include "TouchControls.h"
#include <GLES3/gl3.h>
#include <android/log.h>

namespace ironrift_touch
{
static touchcontrols::TouchControls *controls;

void init()
{
    if (!controls)
        controls = new touchcontrols::TouchControls("ironwail", true, false);
}

void reset()
{
    if (controls)
        controls->resetOutput();
}

bool pointer(int action, int pointer_id, float x, float y)
{
    return controls && controls->processPointer(action, pointer_id, x, y);
}

void draw()
{
    if (!controls)
        return;
    while (glGetError() != GL_NO_ERROR) {}
    GLint program = 0, vao = 0, array_buffer = 0, element_buffer = 0;
    GLint framebuffer = 0, active_texture = 0, texture_2d = 0, sampler = 0;
    GLint viewport[4] = {};
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_VIEWPORT, viewport);
    controls->draw();
    glUseProgram(program);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    glActiveTexture(active_texture);
    glBindTexture(GL_TEXTURE_2D, texture_2d);
    glBindSampler(0, sampler);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        __android_log_print(ANDROID_LOG_ERROR, "IronRiftTouch", "overlay GL error: 0x%x", error);
    GLint restored_program = 0, restored_framebuffer = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &restored_program);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &restored_framebuffer);
    if (restored_program != program || restored_framebuffer != framebuffer)
        __android_log_print(ANDROID_LOG_ERROR, "IronRiftTouch", "overlay state leak: program %d/%d framebuffer %d/%d", restored_program, program, restored_framebuffer, framebuffer);
}
}
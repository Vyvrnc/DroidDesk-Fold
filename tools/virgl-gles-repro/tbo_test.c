// Minimal repro of the OrcaSlicer/libvgcode toolpath draw pattern:
// texture buffer objects (RGBA32F, R32F, R32UI) read with texelFetch in the
// vertex shader + glDrawArraysInstanced. Renders into an FBO via EGL
// surfaceless (desktop GL 4.3 core) and checks pixels with glReadPixels.
//
// usage: tbo_test <variant>
//   full    : RGBA32F pos + RGBA32F color + R32F width + R32UI id, VS fetch, instanced
//   rgba    : only RGBA32F pos TBO, constant color, instanced
//   uint    : only R32UI TBO (usamplerBuffer) selects position + color, instanced
//   r32f    : only R32F TBO gives x offset, instanced
//   noninst : like full but glDrawArrays, instance = gl_VertexID / 6
//   fs      : color fetched from RGBA32F TBO in the fragment shader
//   intattr : libvgcode SegmentTemplate pattern: `in int vertex_id` fed by
//             glVertexAttribIPointer(GL_UNSIGNED_BYTE) + glDrawElementsInstanced(GL_UNSIGNED_BYTE)
// Exit code 0 = pass, 1 = fail.
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static void die(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(2); }

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); exit(2); }
    return s;
}

static GLuint mktbo(GLenum fmt, const void *data, GLsizeiptr size, GLuint *buf_out) {
    GLuint buf, tex;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_TEXTURE_BUFFER, buf);
    glBufferData(GL_TEXTURE_BUFFER, size, data, GL_STATIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    glTexBuffer(GL_TEXTURE_BUFFER, fmt, buf);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    if (buf_out) *buf_out = buf;
    return tex;
}

static const char *VS_HEAD =
    "uniform samplerBuffer pos_tex;\n"     // RGBA32F: x offset, y, ., .
    "uniform samplerBuffer col_tex;\n"     // RGBA32F
    "uniform samplerBuffer width_tex;\n"   // R32F
    "uniform usamplerBuffer id_tex;\n"     // R32UI
    "#ifdef INTATTR\n in int vertex_id;\n#else\n in vec2 in_pos;\n#endif\n"
    "out vec4 v_col;\n"
    "flat out int v_inst;\n";

// Quad covering x in [ox, ox+w], y in [-1,1].
static const char *VS_BODY[] = {
    /* full */
    "void main(){ uint id = texelFetch(id_tex, gl_InstanceID).r;\n"
    " vec4 p = texelFetch(pos_tex, int(id));\n"
    " float w = texelFetch(width_tex, int(id)).r;\n"
    " v_col = texelFetch(col_tex, int(id));\n"
    " gl_Position = vec4(p.x + in_pos.x * w, in_pos.y, 0.0, 1.0); }\n",
    /* rgba */
    "void main(){ vec4 p = texelFetch(pos_tex, gl_InstanceID);\n"
    " v_col = vec4(0.0, 0.0, 1.0, 1.0);\n"
    " gl_Position = vec4(p.x + in_pos.x * 1.0, in_pos.y, 0.0, 1.0); }\n",
    /* uint */
    "void main(){ uint id = texelFetch(id_tex, gl_InstanceID).r;\n"
    " v_col = (id == 7u) ? vec4(1,0,0,1) : (id == 9u ? vec4(0,1,0,1) : vec4(1,1,1,1));\n"
    " gl_Position = vec4(-1.0 + float(gl_InstanceID) + in_pos.x, in_pos.y, 0.0, 1.0); }\n",
    /* r32f */
    "void main(){ float x = texelFetch(width_tex, gl_InstanceID + 2).r;\n"
    " v_col = vec4(1.0, 0.0, 1.0, 1.0);\n"
    " gl_Position = vec4(x + in_pos.x, in_pos.y, 0.0, 1.0); }\n",
    /* noninst */
    "void main(){ int inst = gl_VertexID / 6; uint id = texelFetch(id_tex, inst).r;\n"
    " vec4 p = texelFetch(pos_tex, int(id));\n"
    " float w = texelFetch(width_tex, int(id)).r;\n"
    " v_col = texelFetch(col_tex, int(id));\n"
    " gl_Position = vec4(p.x + in_pos.x * w, in_pos.y, 0.0, 1.0); }\n",
    /* fs */
    "void main(){ v_inst = gl_InstanceID; v_col = vec4(0);\n"
    " gl_Position = vec4(-1.0 + float(gl_InstanceID) + in_pos.x, in_pos.y, 0.0, 1.0); }\n",
    /* intattr */
    "const vec2 C[4] = vec2[](vec2(0,-1), vec2(1,-1), vec2(1,1), vec2(0,1));\n"
    "void main(){ uint id = texelFetch(id_tex, gl_InstanceID).r; vec2 in_pos = C[vertex_id];\n"
    " vec4 p = texelFetch(pos_tex, int(id));\n"
    " float w = texelFetch(width_tex, int(id)).r;\n"
    " v_col = texelFetch(col_tex, int(id));\n"
    " gl_Position = vec4(p.x + in_pos.x * w, in_pos.y, 0.0, 1.0); }\n",
};

static const char *FS_PLAIN =
    "#version 430 core\n in vec4 v_col; flat in int v_inst; out vec4 o; void main(){ o = v_col; }\n";
static const char *FS_TBO =
    "#version 430 core\n uniform samplerBuffer col_tex; in vec4 v_col; flat in int v_inst; out vec4 o;\n"
    " void main(){ o = texelFetch(col_tex, v_inst == 0 ? 1 : 0); }\n";

int main(int argc, char **argv) {
    const char *names[] = {"full", "rgba", "uint", "r32f", "noninst", "fs", "intattr"};
    int v = -1;
    const char *want = argc > 1 ? argv[1] : "full";
    for (int i = 0; i < 7; i++) if (!strcmp(want, names[i])) v = i;
    if (v < 0) die("unknown variant");

    EGLDisplay dpy;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL)) die("eglInitialize");
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfga[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(dpy, cfga, &cfg, 1, &n);
    EGLint ctxa[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                     EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxa);
    if (!ctx) die("eglCreateContext");
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) die("eglMakeCurrent");
    fprintf(stderr, "GL_RENDERER=%s GL_VERSION=%s variant=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), want);

    GLuint fbo, rb;
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("fbo");
    glViewport(0, 0, W, H);

    // data: two segments. id_tex maps instance -> segment id (reversed to test indirection)
    float pos[] = { 0.0f, 0, 0, 1,   -1.0f, 0, 0, 1 };           // seg0 right half, seg1 left half
    float col[] = { 0, 1, 0, 1,   1, 0, 0, 1 };                   // seg0 green, seg1 red
    float width[] = { 1.0f, 1.0f, -1.0f, 0.0f };                  // [0..1] widths, [2..3] x offsets for r32f
    unsigned ids[] = { 1, 0 };
    unsigned ids_uint[] = { 7, 9 };
    GLuint t_pos = mktbo(GL_RGBA32F, pos, sizeof pos, NULL);
    GLuint t_col = mktbo(GL_RGBA32F, col, sizeof col, NULL);
    GLuint t_w = mktbo(GL_R32F, width, sizeof width, NULL);
    GLuint t_id = mktbo(GL_R32UI, v == 2 ? ids_uint : ids, sizeof ids, NULL);

    char vs[4096]; snprintf(vs, sizeof vs, "#version 430 core\n%s%s%s", v == 6 ? "#define INTATTR\n" : "", VS_HEAD, VS_BODY[v]);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, v == 5 ? FS_TBO : FS_PLAIN));
    glBindAttribLocation(prog, 0, "in_pos");
    glLinkProgram(prog);
    GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "link: %s\n", log); return 2; }
    glUseProgram(prog);

    struct { const char *name; GLuint tex; } u[] = {{"pos_tex", t_pos}, {"col_tex", t_col}, {"width_tex", t_w}, {"id_tex", t_id}};
    for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_BUFFER, u[i].tex);
        GLint loc = glGetUniformLocation(prog, u[i].name);
        if (loc >= 0) glUniform1i(loc, i);
    }

    // unit quad x in [0,1], y in [-1,1] (two triangles); for noninst replicate 2x
    float quad[] = { 0,-1, 1,-1, 1,1,  0,-1, 1,1, 0,1 };
    float quad2[24]; memcpy(quad2, quad, sizeof quad); memcpy(quad2 + 12, quad, sizeof quad);
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad2, quad2, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    if (v == 6) {
        static const unsigned char vdata[4] = {0, 1, 2, 3}, idx[6] = {0, 1, 2, 0, 2, 3};
        GLuint ib;
        glBufferData(GL_ARRAY_BUFFER, sizeof vdata, vdata, GL_STATIC_DRAW);
        glVertexAttribIPointer(0, 1, GL_UNSIGNED_BYTE, 0, 0);
        glGenBuffers(1, &ib); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    } else
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (v == 4) glDrawArrays(GL_TRIANGLES, 0, 12);
    else if (v == 6) glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, 0, 2);
    else glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 2);
    GLenum err = glGetError();

    unsigned char px[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unsigned char *L = &px[(32 * W + 16) * 4], *R = &px[(32 * W + 48) * 4];

    // expected left/right colors per variant
    unsigned char exp[7][2][3] = {
        {{255,0,0},{0,255,0}},   // full: inst0->seg1 (left, red), inst1->seg0 (right, green)
        {{0,0,255},{0,0,255}},   // rgba: both blue (inst0 at x=0 right, inst1 at x=-1 left)
        {{255,0,0},{0,255,0}},   // uint: id 7 red left, id 9 green right
        {{255,0,255},{255,0,255}}, // r32f: magenta both halves
        {{255,0,0},{0,255,0}},   // noninst
        {{255,0,0},{0,255,0}},   // fs: inst0 left fetch col[1]=red, inst1 right col[0]=green
        {{255,0,0},{0,255,0}},   // intattr: same as full
    };
    int pass = !err;
    for (int c = 0; c < 3; c++) {
        if (abs(L[c] - exp[v][0][c]) > 2) pass = 0;
        if (abs(R[c] - exp[v][1][c]) > 2) pass = 0;
    }
    printf("%-8s %s  glError=0x%x left=(%d,%d,%d) right=(%d,%d,%d) expected left=(%d,%d,%d) right=(%d,%d,%d)\n",
           want, pass ? "PASS" : "FAIL", err, L[0], L[1], L[2], R[0], R[1], R[2],
           exp[v][0][0], exp[v][0][1], exp[v][0][2], exp[v][1][0], exp[v][1][1], exp[v][1][2]);
    return pass ? 0 : 1;
}

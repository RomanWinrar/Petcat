#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psputils.h>
#include <math.h>
#include <string.h>

PSP_MODULE_INFO("PetCat3D", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272
#define PI 3.14159265f
#define RGB(r,g,b) (0xff000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r))

/* ---------- exit callback ---------- */
static volatile int running = 1;

static int exit_callback(int arg1, int arg2, void *common) {
    running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
}

/* ---------- memory for vertices (rebuilt every frame) ---------- */
static unsigned int __attribute__((aligned(16))) list[262144];
static unsigned char __attribute__((aligned(16))) pool[256 * 1024];
static unsigned int pool_used = 0;

static void *palloc(unsigned int size) {
    size = (size + 15u) & ~15u;
    if (pool_used + size > sizeof(pool)) return NULL;
    void *p = pool + pool_used;
    pool_used += size;
    return p;
}

typedef struct { unsigned int color; float x, y, z; } Vertex3D;
typedef struct { unsigned int color; short x, y, z; } Vertex2D;

static unsigned int rnd_state = 12345u;
static int rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return (int)((rnd_state >> 16) & 0x7fff);
}

static unsigned int shade(unsigned int c, float f) {
    int r = (int)((c & 0xff) * f);
    int g = (int)(((c >> 8) & 0xff) * f);
    int b = (int)(((c >> 16) & 0xff) * f);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return RGB(r, g, b);
}

/* ---------- 3D box ---------- */
static const float face_v[6][4][3] = {
    {{-.5f,-.5f, .5f},{ .5f,-.5f, .5f},{ .5f, .5f, .5f},{-.5f, .5f, .5f}},
    {{ .5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{-.5f, .5f,-.5f},{ .5f, .5f,-.5f}},
    {{ .5f,-.5f, .5f},{ .5f,-.5f,-.5f},{ .5f, .5f,-.5f},{ .5f, .5f, .5f}},
    {{-.5f,-.5f,-.5f},{-.5f,-.5f, .5f},{-.5f, .5f, .5f},{-.5f, .5f,-.5f}},
    {{-.5f, .5f, .5f},{ .5f, .5f, .5f},{ .5f, .5f,-.5f},{-.5f, .5f,-.5f}},
    {{-.5f,-.5f,-.5f},{ .5f,-.5f,-.5f},{ .5f,-.5f, .5f},{-.5f,-.5f, .5f}}
};
static const float face_shade[6] = {1.0f, 0.55f, 0.82f, 0.7f, 1.15f, 0.5f};
static const int tri_idx[6] = {0, 1, 2, 0, 2, 3};

static void box(float x, float y, float z, float sx, float sy, float sz, unsigned int color) {
    Vertex3D *v = (Vertex3D *)palloc(36 * sizeof(Vertex3D));
    if (!v) return;
    int n = 0;
    for (int f = 0; f < 6; f++) {
        unsigned int c = shade(color, face_shade[f]);
        for (int k = 0; k < 6; k++) {
            const float *p = face_v[f][tri_idx[k]];
            v[n].color = c;
            v[n].x = p[0];
            v[n].y = p[1];
            v[n].z = p[2];
            n++;
        }
    }
    sceKernelDcacheWritebackRange(v, 36 * sizeof(Vertex3D));

    sceGumPushMatrix();
    ScePspFVector3 t = { x, y, z };
    sceGumTranslate(&t);
    ScePspFVector3 s = { sx, sy, sz };
    sceGumScale(&s);
    sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 36, 0, v);
    sceGumPopMatrix();
}

static void move(float x, float y, float z) {
    ScePspFVector3 t = { x, y, z };
    sceGumTranslate(&t);
}

/* ---------- 2D HUD ---------- */
static void rect(int x, int y, int w, int h, unsigned int color) {
    Vertex2D *v = (Vertex2D *)palloc(2 * sizeof(Vertex2D));
    if (!v) return;
    v[0].color = color; v[0].x = (short)x;       v[0].y = (short)y;       v[0].z = 0;
    v[1].color = color; v[1].x = (short)(x + w); v[1].y = (short)(y + h); v[1].z = 0;
    sceKernelDcacheWritebackRange(v, 2 * sizeof(Vertex2D));
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

static const unsigned char seg_mask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void digit(int x, int y, int d, int s, unsigned int col) {
    int m = seg_mask[d];
    int L = s * 4;
    if (m & 1)  rect(x + s,     y,                 L, s, col);
    if (m & 2)  rect(x + s + L, y + s,             s, L, col);
    if (m & 4)  rect(x + s + L, y + 2 * s + L,     s, L, col);
    if (m & 8)  rect(x + s,     y + 2 * s + 2 * L, L, s, col);
    if (m & 16) rect(x,         y + 2 * s + L,     s, L, col);
    if (m & 32) rect(x,         y + s,             s, L, col);
    if (m & 64) rect(x + s,     y + s + L,         L, s, col);
}

static void number(int x, int y, int value, int digits, int s, unsigned int col) {
    int max = 1;
    for (int i = 0; i < digits; i++) max *= 10;
    if (value < 0) value = 0;
    if (value >= max) value = max - 1;
    for (int i = digits - 1; i >= 0; i--) {
        digit(x + i * 8 * s, y, value % 10, s, col);
        value /= 10;
    }
}

/* ---------- game state ---------- */
static const unsigned int fur[4]  = { RGB(235,140,50), RGB(150,150,165), RGB(60,60,68), RGB(240,240,240) };
static const unsigned int eyec[4] = { RGB(30,30,30),   RGB(30,30,30),    RGB(250,220,40), RGB(30,30,30) };

typedef struct { float x, y, z, vy; int life; } Heart;
static Heart hearts[16];

static float love = 0.0f, handx = 0.0f, handTilt = 0.0f, roll = 0.0f, rollTarget = 0.0f;
static float yawnAmt = 0.0f;
static int dir = 0, frame = 0;
static int roundStrokes = 0, best = 0, roundFrames = 3600, roundActive = 0;
static int fishCount = 0, fishAnim = 0, chew = 0, furIdx = 0, blinkT = 0, yawnT = 0;

static void spawn_heart(float x) {
    for (int i = 0; i < 16; i++) {
        if (hearts[i].life <= 0) {
            hearts[i].x = x + ((rnd() % 100) / 100.0f - 0.5f) * 0.6f;
            hearts[i].y = 1.9f;
            hearts[i].z = 0.9f;
            hearts[i].vy = 0.03f;
            hearts[i].life = 60;
            break;
        }
    }
}

/* ---------- drawing the scene ---------- */
static void draw_cat(void) {
    unsigned int F = fur[furIdx];
    unsigned int E = eyec[furIdx];
    unsigned int W = RGB(250, 240, 225);
    unsigned int P = RGB(240, 150, 170);
    unsigned int M = RGB(110, 25, 40);
    float t = (float)frame;
    float breathe = 0.02f * sinf(t * 0.05f);
    float shake = (love >= 60.0f) ? sinf(t * 1.9f) * 0.012f * (love / 100.0f) : 0.0f;

    sceGumPushMatrix();
    move(roll * 0.3f + shake, -roll * 0.05f, 0.0f);
    sceGumRotateZ(roll * 1.35f);

    /* body */
    box(0.0f, breathe * 0.5f, 0.0f, 1.5f, 1.3f + breathe, 1.2f, F);
    box(0.0f, -0.1f, 0.62f, 0.62f, 0.8f, 0.04f, W);
    box(-0.78f, -0.39f, -0.05f, 0.35f, 0.8f, 1.0f, F);
    box( 0.78f, -0.39f, -0.05f, 0.35f, 0.8f, 1.0f, F);
    box(-0.3f, -0.57f, 0.62f, 0.34f, 0.45f, 0.36f, W);
    box( 0.3f, -0.57f, 0.62f, 0.34f, 0.45f, 0.36f, W);

    /* tail */
    float wag = t * (0.04f + love * 0.0012f);
    float amp = 0.35f + love * 0.004f;
    for (int i = 0; i < 6; i++) {
        float px = 0.8f + 0.06f * i + sinf(wag - i * 0.7f) * amp * 0.09f * i;
        float py = -0.45f + 0.3f * i;
        float pz = -0.62f - 0.03f * i;
        box(px, py, pz, 0.2f, 0.34f, 0.2f, (i == 5) ? shade(F, 0.65f) : F);
    }

    /* head */
    sceGumPushMatrix();
    move(handx * 0.06f, 0.95f + breathe, 0.2f);
    sceGumRotateY(handx * 0.08f);
    sceGumRotateZ(-handx * 0.05f);
    sceGumRotateX(-yawnAmt * 0.3f);

    box(0.0f, 0.0f, 0.0f, 1.05f, 0.9f, 0.9f, F);
    box(-0.36f, 0.58f, 0.0f, 0.28f, 0.36f, 0.2f, F);
    box( 0.36f, 0.58f, 0.0f, 0.28f, 0.36f, 0.2f, F);
    box(-0.36f, 0.56f, 0.11f, 0.14f, 0.2f, 0.04f, P);
    box( 0.36f, 0.56f, 0.11f, 0.14f, 0.2f, 0.04f, P);

    float eh = 0.22f, ew = 0.2f;
    if (love >= 60.0f || roll > 0.5f) { eh = 0.05f; ew = 0.26f; }
    else if (blinkT > 0)              { eh = 0.03f; }
    else if (fishAnim > 0)            { eh = 0.3f; }
    else if (love < 20.0f)            { eh = 0.1f; }
    box(-0.24f, 0.1f, 0.46f, ew, eh, 0.04f, E);
    box( 0.24f, 0.1f, 0.46f, ew, eh, 0.04f, E);

    box(0.0f, -0.15f, 0.455f, 0.5f, 0.3f, 0.02f, W);
    box(0.0f, -0.08f, 0.47f, 0.12f, 0.09f, 0.06f, P);

    float mouth = yawnAmt * 0.9f;
    if (chew > 0) mouth += 0.35f * fabsf(sinf(chew * 0.5f));
    if (mouth > 0.02f) box(0.0f, -0.27f, 0.47f, 0.28f, 0.05f + mouth * 0.3f, 0.04f, M);

    for (int s = -1; s <= 1; s += 2) {
        for (int j = -1; j <= 1; j++) {
            box(s * 0.62f, -0.14f + j * 0.06f, 0.38f, 0.3f, 0.015f, 0.015f, W);
        }
    }
    sceGumPopMatrix();

    sceGumPopMatrix();
}

static void draw_fish(void) {
    if (fishAnim <= 0) return;
    float t = 1.0f - fishAnim / 45.0f;
    float fx = 2.6f * (1.0f - t);
    float fy = 2.0f + (0.75f - 2.0f) * t + sinf(PI * t) * 0.6f;
    float fz = 1.0f;
    sceGumPushMatrix();
    move(fx, fy, fz);
    sceGumRotateZ(sinf(t * 12.0f) * 0.3f);
    box(0.0f, 0.0f, 0.0f, 0.55f, 0.25f, 0.15f, RGB(90, 160, 230));
    box(0.38f, 0.0f, 0.0f, 0.22f, 0.3f, 0.12f, RGB(60, 120, 200));
    box(-0.18f, 0.04f, 0.08f, 0.06f, 0.06f, 0.02f, RGB(255, 255, 255));
    sceGumPopMatrix();
}

static void draw_hand(void) {
    unsigned int S = RGB(250, 205, 170);
    float hy = 1.85f - roll * 0.45f + 0.04f * sinf(frame * 0.2f);
    sceGumPushMatrix();
    move(handx, hy, 0.85f);
    sceGumRotateZ(handTilt);
    box(0.0f, 0.0f, 0.0f, 0.75f, 0.2f, 0.6f, S);
    for (int i = 0; i < 4; i++) {
        box(-0.27f + 0.18f * i, -0.02f, -0.42f, 0.15f, 0.15f, 0.3f, S);
    }
    box(0.5f, -0.02f, -0.1f, 0.3f, 0.14f, 0.16f, S);
    box(0.0f, 0.12f, 0.8f, 0.5f, 0.3f, 1.0f, RGB(80, 120, 200));
    sceGumPopMatrix();
}

static void draw_hearts(void) {
    for (int i = 0; i < 16; i++) {
        if (hearts[i].life > 0) {
            float s = 0.1f + 0.12f * (hearts[i].life / 60.0f);
            box(hearts[i].x, hearts[i].y, hearts[i].z, s, s, s, RGB(255, 90, 140));
        }
    }
}

static void draw_hud(void) {
    sceGuDisable(GU_DEPTH_TEST);

    /* love bar with heart icon */
    unsigned int pink = RGB(255, 90, 140);
    rect(9, 8, 4, 3, pink);
    rect(15, 8, 4, 3, pink);
    rect(8, 10, 12, 4, pink);
    rect(10, 14, 8, 2, pink);
    rect(12, 16, 4, 2, pink);
    rect(26, 8, 204, 16, RGB(30, 30, 40));
    int w = (int)(love * 2.0f);
    if (w > 200) w = 200;
    if (w < 0) w = 0;
    unsigned int bc = (love < 20.0f) ? RGB(120, 160, 220) : (love < 60.0f ? RGB(240, 170, 200) : pink);
    rect(28, 10, w, 12, bc);

    /* round timer */
    unsigned int gold = RGB(255, 220, 80);
    int sec = (roundFrames + 59) / 60;
    rect(398, 8, 16, 16, gold);
    rect(400, 10, 12, 12, RGB(30, 30, 40));
    rect(405, 12, 2, 6, gold);
    number(430, 8, sec, 2, 2, gold);

    /* fish counter */
    rect(8, 238, 20, 12, RGB(90, 160, 230));
    rect(28, 240, 8, 8, RGB(60, 120, 200));
    number(46, 230, fishCount, 2, 2, RGB(255, 255, 255));

    /* strokes in this round */
    number(218, 230, roundStrokes, 3, 2, RGB(255, 255, 255));

    /* best score */
    rect(370, 236, 14, 14, gold);
    rect(373, 239, 8, 8, RGB(30, 30, 40));
    number(420, 230, best, 3, 2, gold);

    sceGuEnable(GU_DEPTH_TEST);
}

/* ---------- main ---------- */
int main(void) {
    SceCtrlData pad;
    unsigned int oldButtons = 0;

    setup_callbacks();

    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)0x88000, BUF_WIDTH);
    sceGuDepthBuffer((void *)0x110000, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0xc350, 0x2710);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    while (running) {
        sceCtrlPeekBufferPositive(&pad, 1);
        int x = (int)pad.Lx - 128;
        frame++;

        /* strokes */
        int newdir = dir;
        if (x < -50) newdir = -1;
        else if (x > 50) newdir = 1;
        if (dir != 0 && newdir != dir) {
            if (!roundActive) {
                roundActive = 1;
                roundFrames = 3600;
                roundStrokes = 0;
            }
            roundStrokes++;
            love += 8.0f;
            if (love > 100.0f) love = 100.0f;
            spawn_heart(handx);
        }
        dir = newdir;

        if (frame % 6 == 0 && love > 0.0f) love -= 1.0f;
        if (love < 0.0f) love = 0.0f;

        if (roundActive) {
            roundFrames--;
            if (roundFrames <= 0) {
                roundFrames = 0;
                roundActive = 0;
                if (roundStrokes > best) best = roundStrokes;
            }
        }

        /* buttons */
        unsigned int pressed = pad.Buttons & ~oldButtons;
        if ((pressed & PSP_CTRL_CROSS) && fishAnim == 0 && chew == 0) fishAnim = 45;
        if (pressed & PSP_CTRL_TRIANGLE) furIdx = (furIdx + 1) % 4;
        oldButtons = pad.Buttons;

        if (fishAnim > 0) {
            fishAnim--;
            if (fishAnim == 0) {
                love += 25.0f;
                if (love > 100.0f) love = 100.0f;
                fishCount++;
                chew = 50;
            }
        }
        if (chew > 0) chew--;

        /* hand, roll, poses */
        float handTarget = (x / 128.0f) * 1.3f;
        handx += (handTarget - handx) * 0.35f;
        handTilt += (-dir * 0.2f - handTilt) * 0.2f;

        if (love >= 95.0f) rollTarget = 1.0f;
        else if (love < 60.0f) rollTarget = 0.0f;
        roll += (rollTarget - roll) * 0.06f;

        if (blinkT > 0) blinkT--;
        else if (rnd() % 200 == 0) blinkT = 8;

        if (yawnT > 0) yawnT--;
        else if (love < 60.0f && fishAnim == 0 && rnd() % 500 == 0) yawnT = 100;
        yawnAmt = (yawnT > 0) ? sinf(PI * (100 - yawnT) / 100.0f) : 0.0f;

        for (int i = 0; i < 16; i++) {
            if (hearts[i].life > 0) {
                hearts[i].life--;
                hearts[i].y += hearts[i].vy;
            }
        }

        /* render */
        pool_used = 0;
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(RGB(80, 110, 140));
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(55.0f, 16.0f / 9.0f, 0.5f, 100.0f);

        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        {
            ScePspFVector3 cam = { 0.0f, -0.5f, -8.5f };
            sceGumTranslate(&cam);
            sceGumRotateX(0.3f);
        }

        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();

        box(0.0f, -1.25f, 0.0f, 12.0f, 0.4f, 8.0f, RGB(110, 85, 70));
        box(0.0f, -0.92f, 0.1f, 3.6f, 0.25f, 2.8f, RGB(180, 60, 90));

        draw_cat();
        draw_fish();
        draw_hand();
        draw_hearts();
        draw_hud();

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}

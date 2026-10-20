#include "arcballCamera.h"
#include <math.h>

static const float toRadians = (float)(M_PI / 180.0f);

/* Orbit radius: how far the camera sits behind the pivot it orbits. Spawns
   at 0 (the camera sits on the pivot; dragging turns it in place) and scroll
   dollies it out to the max, at which point dragging orbits the pivot. */
float CameraDistance = 0.0f;
const float CameraDistanceMax = 6000.0f;
GLfloat MovementSpeed = 200.0f;
GLfloat CameraScrollSpeed = 10.0f;   // fine: lets you dolly all the way through the atmosphere shell

/* The whole orbit orientation is one unit quaternion: the world -> camera
   rotation. A drag never accumulates per frame; it recomposes from the
   orientation latched at the press, q = q_drag * q_start, so returning the
   cursor to where the drag began returns the view to where it began. */
static quaternion orientation = { 1.0f, 0.0f, 0.0f, 0.0f };
static quaternion dragStartOrientation = { 1.0f, 0.0f, 0.0f, 0.0f };
static vec3 dragStartPos = { 0.0f, 0.0f, 1.0f };
static int dragging = 0;

/* Point the camera orbits / looks at: an absolute world coordinate in
   double, so WASD accumulation never quantizes against the scene scale
   (camera-relative rendering, see getViewMatrix). Starts where the camera
   used to spawn (900 in front of the planet) since the distance starts at 0. */
static dvec3 Pivot = { 0.0, 0.0, 900.0 };

/* Sub-solar surface preset. When active the camera stands just above the
   planet surface (radius 300) at the sub-solar point — the spot where the fixed
   sun (dir normalize(-4000,0,-4000)) is directly overhead — looking across a
   local tangent tilted SURFACE_TILT below the horizon, up = the radial
   direction. That gives a lit daytime interior: blue sky dome up top, bright
   horizon, sunlit fog-ground below. Press G to toggle (see Input.c). */
static int surfaceView = 0;
#define SURFACE_RAD 330.0f
#define SURFACE_TILT 24.0f
static float surfAlt = SURFACE_RAD;

static vec3 Front = {0.0f, 0.0f, -1.0f};
static vec3 Up = {0.0f, 1.0f, 0.0f};
static vec3 Right = {1.0f, 0.0f, 0.0f};

void setCameraPivot(dvec3 pivot)
{
    Pivot = pivot;
}

static quaternion normalizeOrIdentity(quaternion q)
{
    float len = quatLength(q);
    if (len < 1e-6f)
    {
        quaternion identity = { 1.0f, 0.0f, 0.0f, 0.0f };
        return identity;
    }
    return quatNormalize(q);
}

/* Shoemake arcball: the cursor maps onto a unit sphere filling the smaller
   window dimension (so the ball stays round on a wide window). Points off the
   ball snap to its rim, so dragging around the outside rolls about the view
   axis. Width/height are the window's current size in cursor coordinates. */
static vec3 projectCursorToSphere(double xPos, double yPos, int width, int height)
{
    float halfW = (float)width * 0.5f;
    float halfH = (float)height * 0.5f;
    float radius = (halfW < halfH ? halfW : halfH);
    if (radius < 1.0f)
        radius = 1.0f;

    vec3 p;
    p.x = ((float)xPos - halfW) / radius;
    p.y = (halfH - (float)yPos) / radius;
    float d2 = p.x * p.x + p.y * p.y;
    if (d2 <= 1.0f)
    {
        p.z = sqrtf(1.0f - d2);
    }
    else
    {
        float inv = 1.0f / sqrtf(d2);
        p.x *= inv;
        p.y *= inv;
        p.z = 0.0f;
    }
    return p;
}

/* The shortest rotation taking unit vector a onto unit vector b, built
   straight from the half-angle identity (no acos/sin round trip):
   q = normalize(1 + a.b, a x b). */
static quaternion rotationBetween(vec3 a, vec3 b)
{
    vec3 axis = crossvec3(a, b);
    quaternion q = { 1.0f + dotvec3(a, b), axis.x, axis.y, axis.z };
    return normalizeOrIdentity(q);
}

static int  surfaceViewActive(void);
static dvec3 surfaceEye(void);
static mat4 surfaceViewRotation(void);

/* Rotation matrix of the unit orientation quaternion, in the same row layout
   as rotateX/rotateY (m[row][col], applied to column vectors). */
mat4 getViewRotation()
{
    if (surfaceViewActive()) return surfaceViewRotation();
    quaternion q = orientation;
    mat4 r = identityMatrix();
    r.m[0][0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    r.m[0][1] = 2.0f * (q.x * q.y - q.w * q.z);
    r.m[0][2] = 2.0f * (q.x * q.z + q.w * q.y);
    r.m[1][0] = 2.0f * (q.x * q.y + q.w * q.z);
    r.m[1][1] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    r.m[1][2] = 2.0f * (q.y * q.z - q.w * q.x);
    r.m[2][0] = 2.0f * (q.x * q.z - q.w * q.y);
    r.m[2][1] = 2.0f * (q.y * q.z + q.w * q.x);
    r.m[2][2] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    return r;
}

/* Sub-solar "stand on the planet" pose. The sun is fixed along
   normalize(-400,0,-400). We place the camera on the sub-solar point (the spot
   where the sun is directly overhead) and look across a local tangent, tilted
   SURFACE_TILT below the horizon. That yields a sunlit daytime interior: a blue
   sky dome overhead, lit fog-ground below, and a bright horizon between. */
static const vec3 SURFACE_SUN = { -0.7071068f, 0.0f, -0.7071068f };

/* OpenGL lookAt (world -> camera), same row layout as the orbit view. */
static mat4 surfaceLookAt(vec3 eye, vec3 target, vec3 upRef)
{
    vec3 f; f.x = target.x - eye.x; f.y = target.y - eye.y; f.z = target.z - eye.z;
    float fl = sqrtf(f.x*f.x + f.y*f.y + f.z*f.z); f.x /= fl; f.y /= fl; f.z /= fl;
    vec3 r;
    r.x = f.y*upRef.z - f.z*upRef.y;
    r.y = f.z*upRef.x - f.x*upRef.z;
    r.z = f.x*upRef.y - f.y*upRef.x;
    float rl = sqrtf(r.x*r.x + r.y*r.y + r.z*r.z); r.x /= rl; r.y /= rl; r.z /= rl;
    vec3 u;
    u.x = r.y*f.z - r.z*f.y;
    u.y = r.z*f.x - r.x*f.z;
    u.z = r.x*f.y - r.y*f.x;
    mat4 v = identityMatrix();
    v.m[0][0] =  r.x; v.m[0][1] =  r.y; v.m[0][2] =  r.z; v.m[0][3] = -(r.x*eye.x + r.y*eye.y + r.z*eye.z);
    v.m[1][0] =  u.x; v.m[1][1] =  u.y; v.m[1][2] =  u.z; v.m[1][3] = -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
    v.m[2][0] = -f.x; v.m[2][1] = -f.y; v.m[2][2] = -f.z; v.m[2][3] =  (f.x*eye.x + f.y*eye.y + f.z*eye.z);
    v.m[3][0] = 0.0f; v.m[3][1] = 0.0f; v.m[3][2] = 0.0f; v.m[3][3] = 1.0f;
    return v;
}

static int surfaceViewActive(void) { return surfaceView; }

static dvec3 surfaceEye(void)
{
    dvec3 e;
    e.x = (double)SURFACE_SUN.x * surfAlt;
    e.y = (double)SURFACE_SUN.y * surfAlt;
    e.z = (double)SURFACE_SUN.z * surfAlt;
    return e;
}

/* Orientation of the surface pose only: the eye is never baked into a float
   matrix (camera-relative rendering), so the lookAt runs from the origin. */
static mat4 surfaceViewMatrix(void)
{
    vec3 eye = { 0.0f, 0.0f, 0.0f };
    /* A horizontal tangent at the sub-solar point, perpendicular to the sun
       direction in the XZ plane (dot with SURFACE_SUN is 0). */
    vec3 h; h.x =  0.7071068f; h.y = 0.0f; h.z = -0.7071068f;
    float t  = SURFACE_TILT * toRadians;
    float c  = cosf(t);
    float s  = sinf(t);
    /* forward = the horizontal tangent rotated toward "down" (-radial) by t. */
    vec3 f;
    f.x = h.x * c - SURFACE_SUN.x * s;
    f.y = h.y * c - SURFACE_SUN.y * s;
    f.z = h.z * c - SURFACE_SUN.z * s;
    vec3 target;
    target.x = eye.x + f.x;
    target.y = eye.y + f.y;
    target.z = eye.z + f.z;
    vec3 up;
    up.x = SURFACE_SUN.x; up.y = SURFACE_SUN.y; up.z = SURFACE_SUN.z;
    return surfaceLookAt(eye, target, up);
}

static mat4 surfaceViewRotation(void)
{
    /* Upper-left 3x3 of the view matrix; the 4th column stays 0 (rotation). */
    mat4 v = surfaceViewMatrix();
    mat4 r = identityMatrix();
    r.m[0][0] = v.m[0][0]; r.m[0][1] = v.m[0][1]; r.m[0][2] = v.m[0][2];
    r.m[1][0] = v.m[1][0]; r.m[1][1] = v.m[1][1]; r.m[1][2] = v.m[1][2];
    r.m[2][0] = v.m[2][0]; r.m[2][1] = v.m[2][1]; r.m[2][2] = v.m[2][2];
    return r;
}

void toggleSurfaceCamera(void)
{
    surfaceView = !surfaceView;
    /* The surface pose ignores the arcball, so a drag in flight would resume
       against a stale start point when toggling back. */
    dragging = 0;
    if (surfaceView && surfAlt < SURFACE_RAD) surfAlt = SURFACE_RAD;
}

int isSurfaceCamera(void)
{
    return surfaceView;
}

/* Floating-origin view matrix: a pure rotation. Every world position reaching
   the GPU has already had the camera position subtracted on the CPU in double,
   so the camera sits at exactly the origin and no translation (whose size
   would re-quantize against the scene scale) enters the float pipeline.
   Algebraically identical to the old R * translate(-cam) applied to absolute
   positions: R*(v - cam) == R*v_rel. */
mat4 getViewMatrix()
{
    return getViewRotation();
}

/* Absolute world camera position in double: never hand it to the GPU.
   Subtract it from world positions in double and downcast only the
   difference. The orbit camera sits CameraDistance behind the pivot. */
dvec3 getCameraPosition(void)
{
    if (surfaceViewActive()) return surfaceEye();
    mat4 r = getViewRotation();
    dvec3 c;
    c.x = Pivot.x + (double)CameraDistance * r.m[2][0];
    c.y = Pivot.y + (double)CameraDistance * r.m[2][1];
    c.z = Pivot.z + (double)CameraDistance * r.m[2][2];
    return c;
}

/* The view transform is v_cam = R * (v_world + t), so a world direction d
   appears in the camera as R * d. The world direction that shows up as the
   camera's screen +x / +y / -z is a ROW of R, not a column. */
void updateCameraVectors()
{
    mat4 r = getViewRotation();
    vec3 right = { r.m[0][0], r.m[0][1], r.m[0][2] };
    vec3 front = { -r.m[2][0], -r.m[2][1], -r.m[2][2] };
    vec3 up    = { r.m[1][0], r.m[1][1], r.m[1][2] };
    Right = right;
    Front = front;
    Up = up;
}

/* Pans the pivot (and the camera with it) along the view: forward/right are
   the summed key axes in [-1, 1]. The combined direction is normalized so a
   diagonal (W+D) is no faster than a single key. */
void processKeyboard(float forward, float right, GLfloat deltaTime)
{
    if (surfaceViewActive()) return;   // WASD is orbit-pan; ignore in surface mode
    if (forward == 0.0f && right == 0.0f) return;
    updateCameraVectors();

    vec3 dir;
    dir.x = Front.x * forward + Right.x * right;
    dir.y = Front.y * forward + Right.y * right;
    dir.z = Front.z * forward + Right.z * right;
    dir = normalizevec3(dir);

    double velocity = (double)MovementSpeed * deltaTime;
    Pivot = daddvec3(Pivot, (dvec3){dir.x * velocity, dir.y * velocity, dir.z * velocity});
}

/* Press: latch the cursor's point on the ball and the orientation it grabs. */
void arcballBegin(double xpos, double ypos, int width, int height)
{
    if (surfaceViewActive()) return;
    dragStartPos = projectCursorToSphere(xpos, ypos, width, height);
    dragStartOrientation = orientation;
    dragging = 1;
}

/* Move: rotate the scene so the grabbed point follows the cursor across the
   ball, always relative to the press, never to the previous frame. */
void arcballDrag(double xpos, double ypos, int width, int height)
{
    if (!dragging || surfaceViewActive()) return;
    vec3 current = projectCursorToSphere(xpos, ypos, width, height);
    quaternion drag = rotationBetween(dragStartPos, current);
    orientation = normalizeOrIdentity(quatMultiply(drag, dragStartOrientation));
}

/* Release: the orientation already holds the final drag, nothing to commit. */
void arcballEnd(void)
{
    dragging = 0;
}

int arcballDragging(void)
{
    return dragging;
}

/* Scroll in (yoffset < 0) shrinks the orbit radius to 0, out grows it to max.
   In surface mode the same scroll dollies the camera altitude through the
   atmosphere shell (300 -> 445) instead of the orbit radius. */
void processMouseScroll(float yoffset)
{
    if (surfaceViewActive())
    {
        surfAlt += yoffset * CameraScrollSpeed;
        if (surfAlt < SURFACE_RAD) surfAlt = SURFACE_RAD;
        if (surfAlt > 445.0f) surfAlt = 445.0f;
        return;
    }
    CameraDistance += yoffset * CameraScrollSpeed;
    if (CameraDistance < 0.0f)
        CameraDistance = 0.0f;
    if (CameraDistance > CameraDistanceMax)
        CameraDistance = CameraDistanceMax;
}

#include "arcballCamera.h"
#include <math.h>

const float toRadians = (float)(M_PI / 180.0f);

#define ARCBALL_RADIUS 1.0f

/* Orbit radius: how far the camera sits from the pivot the scene rotates
   around. Tuned for the terrain scene (planet r~300 at the origin, sun at
   ~5657). Scroll dollies this between 0 (camera at the pivot) and the max. */
float CameraDistance = 900.0f;
const float CameraDistanceMax = 6000.0f;
GLfloat MovementSpeed = 200.0f;
GLfloat CameraScrollSpeed = 10.0f;   // fine: lets you dolly all the way through the atmosphere shell

/* Quaternion per the arcball article: cosine is the cosine of half the
   rotation angle, axis is the unit rotation axis scaled by the sine of half
   the angle, so q = [cosine, axis] = cos(theta/2) + axis*sin(theta/2). */
typedef struct Quaternion
{
    float cosine;
    vec3 axis;
} Quaternion;

typedef struct Arcball
{
    vec3 startPos;
    vec3 currentPos;
    vec3 startPosUnitVector;
    vec3 currentPosUnitVector;
    Quaternion currentQuaternion;
    Quaternion lastQuaternion;
    float cosValue;
    float cosValue_2;
    float theta;
    float angle;
    vec3 rotationalAxis;
    vec3 rotationalAxis_2;
} Arcball;

/* lastQuaternion starts at the identity quaternion; dragging composes onto it
   relatively. (The article seeds it with [0,(1,0,0)], a 180 deg x-flip that
   only made sense for that default scene orientation.) */
static Arcball arcball = {
    {0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f},
    {0.0f, {1.0f, 0.0f, 0.0f}},
    {1.0f, {0.0f, 0.0f, 0.0f}},
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    {1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f}
};

/* Point the camera orbits / looks at. Defaults to the planet at the origin.
   An absolute world coordinate in double: WASD accumulation never quantizes
   against the scene scale (camera-relative rendering, see getViewMatrix). */
static dvec3 Pivot = { 0.0, 0.0, 0.0 };
static int dragging = 0;

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

vec3 Front = {0.0f, 0.0f, -1.0f};
vec3 Up = {0.0f, 1.0f, 0.0f};
vec3 Right = {1.0f, 0.0f, 0.0f};

void setCameraPivot(dvec3 pivot)
{
    Pivot = pivot;
}

static float zAxis(float x, float y)
{
    float length = sqrtf(x * x + y * y);
    if (length <= ARCBALL_RADIUS)
        return (float)sqrtf(ARCBALL_RADIUS * ARCBALL_RADIUS - x * x - y * y);
    return 0.0f;
}

static void projectCursorToSphere(double xPos, double yPos, vec3 *out)
{
    float windowWidth = getWindowWidth();
    float windowHeight = getWindowHeight();
    out->x = (((float)xPos - (windowWidth / 2.0f)) / (windowWidth / 2.0f)) * ARCBALL_RADIUS;
    out->y = (((windowHeight / 2.0f) - (float)yPos) / (windowHeight / 2.0f)) * ARCBALL_RADIUS;
    out->z = zAxis(out->x, out->y);
}

static void arcballRotation(void)
{
    arcball.startPosUnitVector = normalizevec3(arcball.startPos);
    arcball.currentPosUnitVector = normalizevec3(arcball.currentPos);
    arcball.currentQuaternion.axis = crossvec3(arcball.startPos, arcball.currentPos);
    arcball.currentQuaternion.axis = normalizevec3(arcball.currentQuaternion.axis);

    arcball.cosValue = dotvec3(arcball.startPosUnitVector, arcball.currentPosUnitVector);
    if (arcball.cosValue > 1.0f)
        arcball.cosValue = 1.0f;
    if (arcball.cosValue < -1.0f)
        arcball.cosValue = -1.0f;
    arcball.theta = acosf(arcball.cosValue) * (180.0f / (float)M_PI);

    float halfAngle = (arcball.theta / 2.0f) * toRadians;
    arcball.currentQuaternion.cosine = cosf(halfAngle);
    arcball.currentQuaternion.axis = scalarMultvec3(arcball.currentQuaternion.axis, sinf(halfAngle));

    /* q' = q_current * q_last */
    arcball.cosValue_2 = (arcball.currentQuaternion.cosine * arcball.lastQuaternion.cosine)
        - dotvec3(arcball.currentQuaternion.axis, arcball.lastQuaternion.axis);
    if (arcball.cosValue_2 > 1.0f)
        arcball.cosValue_2 = 1.0f;
    if (arcball.cosValue_2 < -1.0f)
        arcball.cosValue_2 = -1.0f;

    vec3 temporaryVector;
    temporaryVector = crossvec3(arcball.currentQuaternion.axis, arcball.lastQuaternion.axis);

    arcball.rotationalAxis_2.x = (arcball.currentQuaternion.cosine * arcball.lastQuaternion.axis.x)
        + (arcball.lastQuaternion.cosine * arcball.currentQuaternion.axis.x) + temporaryVector.x;
    arcball.rotationalAxis_2.y = (arcball.currentQuaternion.cosine * arcball.lastQuaternion.axis.y)
        + (arcball.lastQuaternion.cosine * arcball.currentQuaternion.axis.y) + temporaryVector.y;
    arcball.rotationalAxis_2.z = (arcball.currentQuaternion.cosine * arcball.lastQuaternion.axis.z)
        + (arcball.lastQuaternion.cosine * arcball.currentQuaternion.axis.z) + temporaryVector.z;

    arcball.angle = (acosf(arcball.cosValue_2) * (180.0f / (float)M_PI)) * 2.0f;

    float halfScale = sinf((arcball.angle / 2.0f) * toRadians);
    if (fabsf(halfScale) > 0.000001f)
        arcball.rotationalAxis = dividevec3(arcball.rotationalAxis_2, halfScale);
    else
    {
        arcball.rotationalAxis.x = 1.0f;
        arcball.rotationalAxis.y = 0.0f;
        arcball.rotationalAxis.z = 0.0f;
    }
}

static int  surfaceViewActive(void);
static dvec3 surfaceEye(void);
static mat4 surfaceViewRotation(void);

mat4 getViewRotation()
{
    if (surfaceViewActive()) return surfaceViewRotation();
    mat4 r = identityMatrix();
    float angle = arcball.angle * toRadians;
    float c = cosf(angle);
    float s = sinf(angle);
    float t = 1.0f - c;
    vec3 axis = normalizevec3(arcball.rotationalAxis);

    /* Rodrigues rotation matrix, same layout as rotateX/rotateY. */
    r.m[0][0] = c + t * axis.x * axis.x;
    r.m[0][1] = t * axis.x * axis.y - s * axis.z;
    r.m[0][2] = t * axis.x * axis.z + s * axis.y;
    r.m[1][0] = t * axis.x * axis.y + s * axis.z;
    r.m[1][1] = c + t * axis.y * axis.y;
    r.m[1][2] = t * axis.y * axis.z - s * axis.x;
    r.m[2][0] = t * axis.x * axis.z - s * axis.y;
    r.m[2][1] = t * axis.y * axis.z + s * axis.x;
    r.m[2][2] = c + t * axis.z * axis.z;
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

void processKeyboard(enum Camera_Movement direction, GLfloat deltaTime, GLfloat deltaSpeed)
{
    if (surfaceViewActive()) return;   // WASD is orbit-pan; ignore in surface mode
    GLfloat velocity = MovementSpeed * deltaTime + deltaSpeed;
    updateCameraVectors();

    if (direction == FORWARD)
        Pivot = daddvec3(Pivot, (dvec3){Front.x * velocity, Front.y * velocity, Front.z * velocity});
    if (direction == BACKWARD)
        Pivot = dsubvec3(Pivot, (dvec3){Front.x * velocity, Front.y * velocity, Front.z * velocity});
    if (direction == LEFT)
        Pivot = dsubvec3(Pivot, (dvec3){Right.x * velocity, Right.y * velocity, Right.z * velocity});
    if (direction == RIGHT)
        Pivot = daddvec3(Pivot, (dvec3){Right.x * velocity, Right.y * velocity, Right.z * velocity});
}

/* terrain's mouse_callback has no discrete press/release event; it reports the
   button state as resetFlag (0 while the button is down, 1 after release).
   Map that onto the arcball's press / move / release by tracking `dragging`. */
void processMouseMovement(float xpos, float ypos, int resetFlag)
{
    projectCursorToSphere(xpos, ypos, &arcball.currentPos);

    if (resetFlag == 0)
    {
        if (!dragging)
        {
            arcball.startPos = arcball.currentPos;
            dragging = 1;
            return;
        }
        arcballRotation();
    }
    else
    {
        if (dragging)
        {
            arcball.lastQuaternion.cosine = arcball.cosValue_2;
            arcball.lastQuaternion.axis = arcball.rotationalAxis_2;
            dragging = 0;
        }
    }
}

/* Scroll in (yoffset < 0) shrinks the orbit radius to 0, out grows it to max.
   In surface mode the same scroll dollies the camera altitude through the
   atmosphere shell (300 -> 445) instead of the orbit radius. */
float processMouseScroll(float yoffset)
{
    if (surfaceViewActive())
    {
        surfAlt += yoffset * CameraScrollSpeed;
        if (surfAlt < SURFACE_RAD) surfAlt = SURFACE_RAD;
        if (surfAlt > 445.0f) surfAlt = 445.0f;
        return 1.0f;
    }
    CameraDistance += yoffset * CameraScrollSpeed;
    if (CameraDistance < 0.0f)
        CameraDistance = 0.0f;
    if (CameraDistance > CameraDistanceMax)
        CameraDistance = CameraDistanceMax;
    return 1.0f;
}

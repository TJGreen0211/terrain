/* A cube-face quadtree LOD sphere. Each cube face is a quadtree of square
   patches addressed by integers (level, x, y), so a node's cube-space
   corners are exact at any depth. The CPU walk per frame subdivides each
   face around the camera: nodes nearer than the (halving) split distance
   are replaced by their 4 children, and every remaining leaf is one
   instance of a shared 10x10 base grid, all faces in a single instanced
   draw.

   Precision (floating origin, see shaders/patch.glsl): each leaf carries
   its four corners on the sphere, computed here in double from the body's
   camera-relative model matrix and only then downcast. Near the camera they
   are small numbers, so the vertex shader places vertices to well under a
   millimetre at any planet size, instead of normalizing on a unit sphere in
   float and scaling that (and its rounding) up by the radius.

   Boundary skirt vertices (position w = 1) duplicate each grid edge
   slightly below the surface so LOD patch edges never crack. */
#include "world/terrain/quad_tree.h"
#include "engine/gfx/buffer.h"
#include "engine/platform/window.h"
#include "world/WorldConfig.h"
#include <stddef.h>

#define QT_DIVISIONS 10
/* Surface cells + outer boundary skirt (4 edges x QT_DIVISIONS segments x 6
   wall verts), kept in sync with buildGrid. */
#define QT_SURFACE_VERTS (QT_DIVISIONS * QT_DIVISIONS * 6)
#define QT_SKIRT_VERTS (4 * QT_DIVISIONS * 6)
#define QT_GRID_VERTS (QT_SURFACE_VERTS + QT_SKIRT_VERTS)
/* Walk nodes per face and drawn leaves per walk. A walk that hits either
   cap stops refining (it never drops a leaf). */
#define QT_NODE_CAP 32768
#define QT_INSTANCE_CAP 65536
/* Hard depth limit (integer node coordinates and the float patch data stay
   exact well past this). */
#define QT_LEVEL_CAP 28
/* Skirt drop cap, body radii. */
#define QT_SKIRT_MAX 0.005

static QuadTreeParams qtParams;
static QuadTree_Stats g_qtStats;

QuadTreeParams * QuadTree_Params(void)
{
	return &qtParams;
}

const QuadTree_Stats * QuadTree_FrameStats(void)
{
	return &g_qtStats;
}

/* One drawn leaf (shaders/patch.glsl, attribute locations 4-11). */
typedef struct qtInstance {
	float corner[4][4];   /* xyz: sphere corners (0,0) (1,0) (0,1) (1,1), camera-relative;
	                         corner[0][3]: skirt drop (body radii) */
	float q0[4];          /* xyz: cube-space corner (0,0); w: cell half-width (cube units) */
	float eu[3];          /* cube-space edge along u */
	float ev[3];          /* cube-space edge along v */
	int addr[4];          /* face, level, x, y: terrain detail lattice (detail.glsl) */
	int nbr[4];           /* levels coarser the neighbour across edges u = 0, u = 1,
	                         v = 0, v = 1 is (0: same or finer); patch.glsl stitches to it */
} qtInstance;

static qtInstance instances[QT_INSTANCE_CAP];
static int instanceNode[QT_INSTANCE_CAP];   /* tree node of each instance */
static int instanceCount;

static GLuint gridVAO, gridVBO, gridEBO, instanceVBO;

/* Cube faces: face point (s, t) in [0,1]^2 is O + s*A + t*B, with A x B
   along the outward normal, so (u, v) triangles wound counter-clockwise in
   the grid face outward. */
static const double faceO[6][3] = {
	{ 1, -1, -1}, {-1, -1, -1},   /* +X, -X */
	{-1,  1, -1}, {-1, -1, -1},   /* +Y, -Y */
	{-1, -1,  1}, {-1, -1, -1}    /* +Z, -Z */
};
static const double faceA[6][3] = {
	{0, 2, 0}, {0, 0, 2},
	{0, 0, 2}, {2, 0, 0},
	{2, 0, 0}, {0, 2, 0}
};
static const double faceB[6][3] = {
	{0, 0, 2}, {0, 2, 0},
	{2, 0, 0}, {0, 0, 2},
	{0, 2, 0}, {2, 0, 0}
};

/* Cube-space point at node coordinates (x, y) of a level-`level` grid on
   face f (fractional x, y address points inside a node). Exact in double:
   sizes are powers of two. */
static dvec3 cubePoint(int f, int level, double x, double y)
{
	double size = ldexp(1.0, -level);
	double s = x * size, t = y * size;
	dvec3 q = {
		faceO[f][0] + s * faceA[f][0] + t * faceB[f][0],
		faceO[f][1] + s * faceA[f][1] + t * faceB[f][1],
		faceO[f][2] + s * faceA[f][2] + t * faceB[f][2]
	};
	return q;
}

/* Camera-relative point on the sphere for cube point q: model *
   normalize(q), with model = translate(body - camera) * scale * rotation
   (column vectors, translation in column 3). */
static dvec3 spherePoint(const dmat4 *model, dvec3 q)
{
	double inv = 1.0 / sqrt(q.x*q.x + q.y*q.y + q.z*q.z);
	double d[3] = { q.x * inv, q.y * inv, q.z * inv };
	const double (*m)[4] = model->m;
	dvec3 p = {
		m[0][0]*d[0] + m[0][1]*d[1] + m[0][2]*d[2] + m[0][3],
		m[1][0]*d[0] + m[1][1]*d[1] + m[1][2]*d[2] + m[1][3],
		m[2][0]*d[0] + m[2][1]*d[1] + m[2][2]*d[2] + m[2][3]
	};
	return p;
}

/* ---- Base grid ----------------------------------------------------------- */

/* Grid vertex (u, v) in [0,1]^2 at indices (iu, iv); w is the skirt flag
   (0 = surface, 1 = wall). The vertex shaders map (u, v) onto the patch. */
static vec4 gridPoint(int iu, int iv, float wall)
{
	vec4 p = { (float)iu / QT_DIVISIONS, (float)iv / QT_DIVISIONS, 0.0f, wall };
	return p;
}

static int buildGrid(vec4 *out)
{
	int n = 0;
	for(int iv = 0; iv < QT_DIVISIONS; iv++) {
		for(int iu = 0; iu < QT_DIVISIONS; iu++) {
			vec4 a = gridPoint(iu, iv, 0.0f), b = gridPoint(iu+1, iv, 0.0f);
			vec4 c = gridPoint(iu, iv+1, 0.0f), d = gridPoint(iu+1, iv+1, 0.0f);
			out[n++] = a; out[n++] = b; out[n++] = c;
			out[n++] = b; out[n++] = d; out[n++] = c;
		}
	}

	/* Boundary skirt: walk the grid perimeter once and duplicate each boundary
	   point as a wall vertex (same uv, w = 1); the vertex shader pulls the
	   wall ring radially below the lowest possible displaced surface, so the
	   wall hides T-junction and mip mismatches at every patch edge. Culling
	   is off in this pass, so the winding is irrelevant. */
	static const int edgeStart[4][2] = {
		{0, 0}, {QT_DIVISIONS, 0}, {QT_DIVISIONS, QT_DIVISIONS}, {0, QT_DIVISIONS}
	};
	static const int edgeStep[4][2] = {
		{1, 0}, {0, 1}, {-1, 0}, {0, -1}
	};
	for(int e = 0; e < 4; e++) {
		for(int s = 0; s < QT_DIVISIONS; s++) {
			int u0 = edgeStart[e][0] + edgeStep[e][0]*s, v0 = edgeStart[e][1] + edgeStep[e][1]*s;
			int u1 = u0 + edgeStep[e][0], v1 = v0 + edgeStep[e][1];
			vec4 b0 = gridPoint(u0, v0, 0.0f), b1 = gridPoint(u1, v1, 0.0f);
			vec4 s0 = gridPoint(u0, v0, 1.0f), s1 = gridPoint(u1, v1, 1.0f);
			out[n++] = b0; out[n++] = b1; out[n++] = s0;
			out[n++] = s0; out[n++] = b1; out[n++] = s1;
		}
	}
	return n;
}

/* Collapses the triangle list into unique vertices + 16-bit indices. Only
   bit-identical vertices merge, so the drawn mesh is exactly the same; the
   vertex shader (height fetch, atan/asin) just runs ~5x less per instance
   thanks to the post-transform cache. Returns the unique vertex count. */
static int indexGrid(const vec4 *tris, int count, vec4 *uniq, GLushort *idx)
{
	int nu = 0;
	for(int i = 0; i < count; i++) {
		int j = 0;
		while(j < nu && memcmp(&uniq[j], &tris[i], sizeof(vec4)) != 0)
			j++;
		if(j == nu)
			uniq[nu++] = tris[i];
		idx[i] = (GLushort)j;
	}
	return nu;
}

void QuadTree_InitGL(void)
{
	vec4 verts[QT_GRID_VERTS], uniq[QT_GRID_VERTS];
	GLushort indices[QT_GRID_VERTS];
	buildGrid(verts);
	int uniqueCount = indexGrid(verts, QT_GRID_VERTS, uniq, indices);

	glGenVertexArrays(1, &gridVAO);
	glBindVertexArray(gridVAO);

	glGenBuffers(1, &gridVBO);
	glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
	glBufferData(GL_ARRAY_BUFFER, uniqueCount*sizeof(vec4), uniq, GL_STATIC_DRAW);
	/* Element buffer binding is VAO state: bound here, it stays with gridVAO. */
	glGenBuffers(1, &gridEBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gridEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
	/* vPosition (location 0 in every body program): (u, v, 0, skirt). */
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4*sizeof(float), BUFFER_OFFSET(0));

	/* Patch instance attributes, locations 4-12 (shaders/patch.glsl). */
	glGenBuffers(1, &instanceVBO);
	glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(qtInstance)*QT_INSTANCE_CAP, NULL, GL_STREAM_DRAW);
	GLsizei stride = sizeof(qtInstance);
	for(int k = 0; k < 4; k++) {
		glEnableVertexAttribArray(4+k);
		glVertexAttribPointer(4+k, 4, GL_FLOAT, GL_FALSE, stride, BUFFER_OFFSET(offsetof(qtInstance, corner) + k*4*sizeof(float)));
		glVertexAttribDivisor(4+k, 1);
	}
	glEnableVertexAttribArray(8);
	glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, stride, BUFFER_OFFSET(offsetof(qtInstance, q0)));
	glVertexAttribDivisor(8, 1);
	glEnableVertexAttribArray(9);
	glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE, stride, BUFFER_OFFSET(offsetof(qtInstance, eu)));
	glVertexAttribDivisor(9, 1);
	glEnableVertexAttribArray(10);
	glVertexAttribPointer(10, 3, GL_FLOAT, GL_FALSE, stride, BUFFER_OFFSET(offsetof(qtInstance, ev)));
	glVertexAttribDivisor(10, 1);
	glEnableVertexAttribArray(11);
	glVertexAttribIPointer(11, 4, GL_INT, stride, BUFFER_OFFSET(offsetof(qtInstance, addr)));
	glVertexAttribDivisor(11, 1);
	glEnableVertexAttribArray(12);
	glVertexAttribIPointer(12, 4, GL_INT, stride, BUFFER_OFFSET(offsetof(qtInstance, nbr)));
	glVertexAttribDivisor(12, 1);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}


/* ---- Walk ------------------------------------------------------------------ */

/* Per-walk inputs shared by every face, all camera-relative double (the
   camera is the origin). */
typedef struct qtWalk {
	const QuadTree_Body *body;
	dvec3 center;           /* body centre */
	double radius;
	double splitK;          /* split while distance < splitK * node width */
	int maxLevel;
	int useHorizon;
	int cullNear;           /* 0: skip the frustum's near plane (shadow cascades) */
	double camDist;         /* |centre| */
	dvec3 camDir;           /* unit, centre -> camera */
	double occluder;        /* horizon occluder radius, m */
	double frustum[6][4];   /* ax + by + cz + d >= 0 inside */
	int id;                 /* stamps the tree nodes this walk splits and draws */
} qtWalk;

static double dotd(dvec3 a, dvec3 b)
{
	return a.x*b.x + a.y*b.y + a.z*b.z;
}

static dvec3 normd(dvec3 q)
{
	return dscalarvec3(q, 1.0 / dlengthvec3(q));
}

/* Deepest level whose grid vertices are still >= minSpacing apart: a root
   face spans ~R*pi/2 with QT_DIVISIONS cells. */
static int levelForSpacing(double radius, double minSpacing)
{
	double cells = radius * 0.5 * M_PI / (QT_DIVISIONS * fmax(minSpacing, 1e-3));
	int level = cells > 1.0 ? (int)ceil(log2(cells)) : 0;
	return level > QT_LEVEL_CAP ? QT_LEVEL_CAP : level;
}

/* At the split distance a node of width L spans L/(K L) of the view, so one
   of its QT_DIVISIONS triangles covers H / (QT_DIVISIONS * K * 2 tan(fov/2))
   pixels: solve for K. */
double QuadTree_SplitK(double trianglePx)
{
	double tanHalf = tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0);
	return Window_Height() / (QT_DIVISIONS * trianglePx * 2.0 * tanHalf);
}

double QuadTree_LeafCellK(double trianglePx)
{
	return 1.0 / (QT_DIVISIONS * QuadTree_SplitK(trianglePx));
}

static void buildWalk(qtWalk *w, const QuadTree_Body *body, mat4 viewProj, int useHorizon, int cullNear,
                      double minSpacing, double trianglePx)
{
	w->cullNear = cullNear;
	const double (*m)[4] = body->model.m;
	w->body = body;
	w->center.x = m[0][3]; w->center.y = m[1][3]; w->center.z = m[2][3];
	w->radius = body->radius;
	w->maxLevel = levelForSpacing(body->radius, minSpacing);

	w->splitK = QuadTree_SplitK(trianglePx);

	/* Horizon: the lowest solid surface (seafloor; water is see-through)
	   hides everything past acos(occ/d), and a node reaching radius r peeks
	   over it by a further acos(occ/r). Off inside the occluder. */
	w->camDist = dlengthvec3(w->center);
	w->occluder = body->radius * (1.0 + fmin(body->occluderRelief, 0.0));
	w->useHorizon = useHorizon && w->camDist > w->occluder;
	w->camDir = dscalarvec3(w->center, -1.0 / fmax(w->camDist, 1e-9));

	/* Gribb-Hartmann planes from clip = viewProj * world (column vectors,
	   rows m[i][*]): left/right, bottom/top, near/far. */
	for(int p = 0; p < 6; p++) {
		int row = p >> 1;
		double sign = (p & 1) ? -1.0 : 1.0;
		for(int k = 0; k < 4; k++)
			w->frustum[p][k] = (double)viewProj.m[3][k] + sign*(double)viewProj.m[row][k];
		double len = sqrt(w->frustum[p][0]*w->frustum[p][0] + w->frustum[p][1]*w->frustum[p][1] + w->frustum[p][2]*w->frustum[p][2]);
		if(len > 0.0)
			for(int k = 0; k < 4; k++)
				w->frustum[p][k] /= len;
	}
}

/* Skirt drop (body radii) of a patch `width` metres wide whose coarse
   surface spans `range` (body radii) and reaches `hi`: a seam opens at
   most as far as heights differ along the edge (QT_SKIRT_*, WorldConfig.h),
   plus `seam` (body radii) for terrain detail, whose amplitude the range
   leaves out (it bounds the absolute height, not how far it can differ
   across an edge). Shipped per patch to the shaders (patchSkirt), so the
   culling bounds and the walls agree. */
static double skirtDrop(double range, double hi, double seam, double width, double radius)
{
	double drop = QT_SKIRT_RANGE_FACTOR * range + QT_SKIRT_WIDTH_FRACTION * width / radius + seam;
	return fmin(drop, QT_SKIRT_MAX * (1.0 + hi));
}

/* ---- Shared LOD tree ------------------------------------------------------
   The split rule depends only on the camera, so every walk of a body at the
   same LOD target (the main pass, each shadow cascade) refines the same tree
   and differs only in what it culls and how deep it may go. The tree is
   built lazily: a node's footprint, relief bounds, skirt and split decision
   are computed the first time any walk reaches it, then reused by the
   others. It lives until its inputs change (in practice every frame, as the
   camera moves; QuadTree_BeginFrame also drops it, so live relief edits
   take effect). */

typedef struct qtTreeNode {
	int f, level, x, y;
	int built;              /* bounds below computed */
	int empty;              /* relief: nothing drawn over the footprint */
	int split;              /* the split rule wants children (before a walk's maxLevel) */
	int child;              /* first of 4 consecutive children, -1 = not created */
	int inst;               /* leafCache index, -1 = not built */
	int splitWalk;          /* id of the last walk that split this node */
	int leafWalk;           /* id of the last walk that drew it as a leaf */
	double skirt;           /* body radii */
	dvec3 s;                /* frustum sphere centre, camera-relative */
	double bound;           /* and radius */
	dvec3 up;               /* footprint centre direction (world) */
	double capAngle, rTop;  /* horizon test */
} qtTreeNode;

#define QT_TREE_CAP 65536
static qtTreeNode tree[QT_TREE_CAP];
static int treeCount;
static qtInstance leafCache[QT_TREE_CAP];
static int leafCount;

/* What the tree depends on: walks with equal keys share it. */
typedef struct qtTreeKey {
	dmat4 model;
	double radius, trianglePx, detailAmp, detailSeam;
	QuadTree_ReliefFn relief;
	float windowHeight;
} qtTreeKey;
static qtTreeKey treeKey;
static int treeValid;

static void initTreeNode(qtTreeNode *c, int f, int level, int x, int y)
{
	c->f = f; c->level = level; c->x = x; c->y = y;
	c->built = 0;
	c->child = -1;
	c->inst = -1;
	c->splitWalk = 0;
	c->leafWalk = 0;
}

static void useTree(const QuadTree_Body *body, double trianglePx)
{
	qtTreeKey k;
	memset(&k, 0, sizeof k);   /* padding too: keys compare with memcmp */
	k.model = body->model;
	k.radius = body->radius;
	k.trianglePx = trianglePx;
	k.detailAmp = body->detailAmp;
	k.detailSeam = body->detailSeam;
	k.relief = body->relief;
	k.windowHeight = Window_Height();
	if(treeValid && memcmp(&k, &treeKey, sizeof k) == 0)
		return;
	treeKey = k;
	treeValid = 1;
	leafCount = 0;
	for(int f = 0; f < 6; f++)
		initTreeNode(&tree[f], f, 0, 0, 0);
	treeCount = 6;
}

/* Fills in a leaf instance: its four sphere corners in double,
   camera-relative, then downcast (shaders/patch.glsl rebuilds the surface
   from them), and its skirt drop (body radii). */
static void buildLeaf(qtInstance *inst, const dmat4 *model, const qtTreeNode *c)
{
	int f = c->f;
	double size = ldexp(1.0, -c->level);

	for(int i = 0; i < 4; i++) {
		dvec3 p = spherePoint(model, cubePoint(f, c->level, c->x + (i & 1), c->y + (i >> 1)));
		inst->corner[i][0] = (float)p.x;
		inst->corner[i][1] = (float)p.y;
		inst->corner[i][2] = (float)p.z;
		inst->corner[i][3] = 0.0f;
	}
	inst->corner[0][3] = (float)c->skirt;
	dvec3 q0 = cubePoint(f, c->level, c->x, c->y);
	inst->q0[0] = (float)q0.x;
	inst->q0[1] = (float)q0.y;
	inst->q0[2] = (float)q0.z;
	/* Cell half-width in cube units: the vertex shaders derive the
	   height-map mip from it. */
	inst->q0[3] = (float)size;
	for(int k = 0; k < 3; k++) {
		inst->eu[k] = (float)(faceA[f][k] * size);
		inst->ev[k] = (float)(faceB[f][k] * size);
	}
	inst->addr[0] = f;
	inst->addr[1] = c->level;
	inst->addr[2] = c->x;
	inst->addr[3] = c->y;
}

/* Computes a tree node's bounds from its footprint on the unit sphere
   (body-local, exact in double) and the body's relief over it:
   - frustum: a sphere around the node's height shell;
   - horizon: the node's highest point against the occluder;
   - split: distance to the node's vertical extent (so standing on a
     mountain refines the mountain), against K node widths. */
static void buildNode(const qtWalk *w, qtTreeNode *c)
{
	const QuadTree_Body *b = w->body;
	double R = w->radius;
	double cell = ldexp(1.0, -c->level);
	int f = c->f;

	c->built = 1;
	c->empty = 0;
	c->split = 0;

	/* Footprint: centre direction, corner directions, cap and width. */
	dvec3 qc = cubePoint(f, c->level, c->x + 0.5, c->y + 0.5);
	dvec3 nc = normd(qc);
	dvec3 n[4];
	double chord = 0.0;
	for(int i = 0; i < 4; i++) {
		n[i] = normd(cubePoint(f, c->level, c->x + (i & 1), c->y + (i >> 1)));
		chord = fmax(chord, dlengthvec3(dsubvec3(n[i], nc)));
	}
	double capAngle = 2.0 * asin(fmin(0.5 * chord, 1.0));
	double width = R * fmax(dlengthvec3(dsubvec3(n[0], n[1])), dlengthvec3(dsubvec3(n[0], n[2])));

	/* Camera-relative sea-level centre and world up. */
	dvec3 sea = spherePoint(&b->model, qc);
	dvec3 up = dscalarvec3(dsubvec3(sea, w->center), 1.0 / R);

	double dirv[3] = { nc.x, nc.y, nc.z };
	double maxDist = dlengthvec3(sea) + R * chord;
	double lo = 0.0, hi = 0.0;
	if(b->relief) {
		b->relief(dirv, capAngle, cell, maxDist, &lo, &hi);
		if(lo > hi) {
			c->empty = 1;   /* nothing drawn over this footprint */
			return;
		}
	}
	/* Terrain detail on land (relief > 0): up to detailAmp either way, but
	   never below sea level (detail.glsl's coastal envelope). */
	double range = hi - lo, seam = 0.0;
	if(b->detailAmp > 0.0 && hi > 0.0) {
		if(lo > 0.0)
			lo = fmax(lo - b->detailAmp, 0.0);
		hi += b->detailAmp;
		seam = b->detailSeam * width / R;
	}
	c->skirt = skirtDrop(range, hi, seam, width, R);
	double loS = lo - c->skirt;

	/* Frustum sphere around the shell [R(1+loS), R(1+hi)] over the cap. A
	   point at radius r, angle a from the centre line, lies within
	   sqrt((r - mid)^2 + r*mid*chord^2) of the mid point. */
	double mid = R * (1.0 + 0.5 * (loS + hi));
	c->rTop = R * (1.0 + hi);
	double half = 0.5 * R * (hi - loS);
	c->bound = sqrt(half*half + c->rTop * mid * chord * chord);
	c->s = daddvec3(w->center, dscalarvec3(up, mid));
	c->up = up;
	c->capAngle = capAngle;

	/* Closest point of the node's vertical extent through its centre. */
	double t = fmax(fmin(-dotd(w->center, up), R * (1.0 + hi)), R * (1.0 + lo));
	double dist = dlengthvec3(daddvec3(w->center, dscalarvec3(up, t)));
	c->split = dist < w->splitK * width;
}

/* 1 if this walk culls the node: outside its frustum, or (main pass) past
   the horizon. */
static int culled(const qtWalk *w, const qtTreeNode *c)
{
	for(int p = 0; p < 6; p++) {
		if(p == 4 && !w->cullNear)
			continue;   /* near plane (row 3 + row 2) */
		const double *pl = w->frustum[p];
		if(pl[0]*c->s.x + pl[1]*c->s.y + pl[2]*c->s.z + pl[3] < -c->bound)
			return 1;
	}
	if(w->useHorizon) {
		double cosA = fmax(fmin(dotd(c->up, w->camDir), 1.0), -1.0);
		double limit = acos(fmin(w->occluder / w->camDist, 1.0))
		             + acos(fmin(w->occluder / c->rTop, 1.0));
		if(acos(cosA) - c->capAngle > limit)
			return 1;
	}
	return 0;
}

/* One walk over a single face of the shared tree (breadth first, creating
   and building nodes on first use): appends the visible leaf instances
   (split parents and culled nodes emit nothing). */
static void traverseFace(const qtWalk *w, int f)
{
	static int queue[QT_NODE_CAP];

	queue[0] = f;   /* the face's root */
	int numNodes = 1;

	for(int k = 0; k < numNodes; k++) {
		qtTreeNode *c = &tree[queue[k]];
		if(!c->built) {
			buildNode(w, c);
			g_qtStats.built++;
		}
		g_qtStats.nodes++;
		if(c->empty || culled(w, c))
			continue;

		/* Room for this node's children, and for every node already
		   queued to become a leaf. */
		int canSplit = numNodes + 4 <= QT_NODE_CAP
		            && instanceCount + (numNodes - k) + 4 <= QT_INSTANCE_CAP
		            && (c->child >= 0 || treeCount + 4 <= QT_TREE_CAP);
		if(canSplit && c->split && c->level < w->maxLevel) {
			c->splitWalk = w->id;
			if(c->child < 0) {
				c->child = treeCount;
				for(int j = 0; j < 4; j++)
					initTreeNode(&tree[treeCount + j], f, c->level + 1,
					             2*c->x + (j & 1), 2*c->y + (j >> 1));
				treeCount += 4;
			}
			for(int j = 0; j < 4; j++)
				queue[numNodes++] = c->child + j;
			continue;
		}
		if(instanceCount < QT_INSTANCE_CAP) {
			if(c->inst < 0 && leafCount < QT_TREE_CAP) {
				c->inst = leafCount++;
				buildLeaf(&leafCache[c->inst], &w->body->model, c);
			}
			if(c->inst >= 0)
				instances[instanceCount] = leafCache[c->inst];
			else
				buildLeaf(&instances[instanceCount], &w->body->model, c);
			instanceNode[instanceCount++] = queue[k];
			c->leafWalk = w->id;
			if(c->level > g_qtStats.maxLevel)
				g_qtStats.maxLevel = c->level;
		}
	}
}

/* Face holding cube-space direction q and its face coordinates (s, t) in
   [0,1]^2 (cubePoint's inverse at level 0). */
static int faceOf(dvec3 q, double *s, double *t)
{
	double a[3] = { q.x, q.y, q.z };
	int axis = 0;
	for(int i = 1; i < 3; i++)
		if(fabs(a[i]) > fabs(a[axis]))
			axis = i;
	int f = 2 * axis + (a[axis] < 0.0);
	double inv = 1.0 / fabs(a[axis]);
	double p[3];
	for(int i = 0; i < 3; i++)
		p[i] = a[i] * inv - faceO[f][i];
	*s = 0.25 * (p[0]*faceA[f][0] + p[1]*faceA[f][1] + p[2]*faceA[f][2]);
	*t = 0.25 * (p[0]*faceB[f][0] + p[1]*faceB[f][1] + p[2]*faceB[f][2]);
	return f;
}

/* How many levels coarser the leaf this walk drew across edge e (u = 0,
   u = 1, v = 0, v = 1) of leaf c is; 0 when it is as fine or finer, or not
   drawn. The tree has no 2:1 balance, and the split test measures to each
   node's vertical extent, so a mountain coast can sit several levels finer
   than the sea patch beside it. The shaders lay such an edge onto the
   coarse neighbour's (patch.glsl patchStitch), or the coarse edge cuts
   straight across terrain the fine side follows and its skirt shows as a
   wall. The probe is a quarter node past the edge midpoint, found by
   direction so it works across cube faces too: off the face plane there,
   its direction lands in the neighbouring face's matching node. */
static int coarserGap(const qtWalk *w, const qtTreeNode *c, int e)
{
	static const double probe[4][2] = { {-0.25, 0.5}, {1.25, 0.5}, {0.5, -0.25}, {0.5, 1.25} };
	double s, t;
	int f = faceOf(cubePoint(c->f, c->level, c->x + probe[e][0], c->y + probe[e][1]), &s, &t);
	int span = 1 << c->level;
	int ix = (int)floor(s * span), iy = (int)floor(t * span);
	ix = ix < 0 ? 0 : (ix >= span ? span - 1 : ix);
	iy = iy < 0 ? 0 : (iy >= span ? span - 1 : iy);
	const qtTreeNode *n = &tree[f];
	while(n->level < c->level && n->splitWalk == w->id) {
		int sh = c->level - n->level - 1;
		n = &tree[n->child + (((ix >> sh) & 1) | (((iy >> sh) & 1) << 1))];
	}
	return (n->level < c->level && n->leafWalk == w->id) ? c->level - n->level : 0;
}

/* Last walk's inputs: a repeated draw of the same body with the same
   inputs reuses the instance buffer already uploaded. QuadTree_BeginFrame
   drops it, so live relief edits (bounds) always take effect next frame. */
typedef struct qtKey {
	dmat4 model;
	double radius, occluderRelief, minSpacing, trianglePx, detailAmp, detailSeam;
	QuadTree_ReliefFn relief;
	mat4 viewProj;
	int horizon, cullNear;
	float windowHeight;
} qtKey;
static qtKey lastKey;
static int lastValid;

static qtKey makeKey(const QuadTree_Body *body, mat4 viewProj, int useHorizon, int cullNear,
                     double minSpacing, double trianglePx)
{
	qtKey k;
	memset(&k, 0, sizeof k);   /* padding too: keys compare with memcmp */
	k.model = body->model;
	k.radius = body->radius;
	k.occluderRelief = body->occluderRelief;
	k.minSpacing = minSpacing;
	k.trianglePx = trianglePx;
	k.detailAmp = body->detailAmp;
	k.detailSeam = body->detailSeam;
	k.relief = body->relief;
	k.viewProj = viewProj;
	k.horizon = useHorizon;
	k.cullNear = cullNear;
	k.windowHeight = Window_Height();
	return k;
}

static void walkFaces(const QuadTree_Body *body, mat4 viewProj, int useHorizon, int cullNear,
                      double minSpacing, double trianglePx)
{
	qtKey key = makeKey(body, viewProj, useHorizon, cullNear, minSpacing, trianglePx);
	if(lastValid && memcmp(&key, &lastKey, sizeof key) == 0)
		return;

	LARGE_INTEGER t0, t1, freq;
	QueryPerformanceCounter(&t0);

	static int walkId;
	qtWalk w;
	buildWalk(&w, body, viewProj, useHorizon, cullNear, minSpacing, trianglePx);
	w.id = ++walkId;
	useTree(body, trianglePx);
	instanceCount = 0;
	for(int f = 0; f < 6; f++)
		traverseFace(&w, f);
	for(int i = 0; i < instanceCount; i++)
		for(int e = 0; e < 4; e++)
			instances[i].nbr[e] = coarserGap(&w, &tree[instanceNode[i]], e);

	if(instanceCount > 0) {
		/* Full re-specify orphans the old storage, so a draw still reading
		   the previous body's instances never stalls us. */
		glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof(qtInstance)*instanceCount, instances, GL_STREAM_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&freq);
	g_qtStats.walks++;
	g_qtStats.leaves += instanceCount;
	g_qtStats.walkMs += 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;

	lastKey = key;
	lastValid = 1;
}

static void drawPatches(void)
{
	if(instanceCount == 0)
		return;

	/* Per-draw uniforms of whichever body program is bound (patch.glsl). */
	GLint program = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glUniform1i(glGetUniformLocation((GLuint)program, "patchLegacy"), qtParams.legacyPath);

	/* Patch skirts must render double-sided, so culling is off entirely for
	   this pass and the depth buffer resolves the near/far hemispheres (the
	   planets pass is opaque). Wireframe therefore also shows the far
	   hemisphere's grid and walls. Hand the normal GL_BACK state back to the
	   rest of the frame afterwards. */
	glDisable(GL_CULL_FACE);
	glBindVertexArray(gridVAO);
	/* Every patch's surface first, then every skirt: the walls hang below
	   surfaces that are by then in the depth buffer, so their (expensive,
	   mostly hidden) fragments are rejected before shading. buildGrid puts
	   the skirt triangles after the surface cells. */
	glDrawElementsInstanced(GL_TRIANGLES, QT_SURFACE_VERTS, GL_UNSIGNED_SHORT, BUFFER_OFFSET(0), instanceCount);
	glDrawElementsInstanced(GL_TRIANGLES, QT_SKIRT_VERTS, GL_UNSIGNED_SHORT,
	                        BUFFER_OFFSET(QT_SURFACE_VERTS * sizeof(GLushort)), instanceCount);
	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
}

void QuadTree_BeginFrame(void)
{
	lastValid = 0;
	treeValid = 0;
	memset(&g_qtStats, 0, sizeof g_qtStats);
}

void QuadTree_DrawShadowCasters(const QuadTree_Body *body, mat4 lightViewProj, double minSpacing,
                                double trianglePx, int cullNear)
{
	/* Camera-centred LOD like the main pass (the split depends only on the
	   camera, so at the same trianglePx the leaves that survive culling are
	   exactly the main pass's), culled against the light frustum, and
	   without the camera horizon cull: terrain past the horizon can still
	   shadow what the camera sees. */
	walkFaces(body, lightViewProj, 0, cullNear, fmax(minSpacing, QT_MIN_VERTEX_SPACING_M), trianglePx);
	drawPatches();
}

void QuadTree_DrawSphere(const QuadTree_Body *body, mat4 viewProj)
{
	walkFaces(body, viewProj, 1, 1, QT_MIN_VERTEX_SPACING_M,
	          body->trianglePx > 0.0 ? body->trianglePx : QT_TARGET_TRIANGLE_PX);
	drawPatches();
}

void QuadTree_Shutdown(void)
{
	glDeleteVertexArrays(1, &gridVAO);
	glDeleteBuffers(1, &gridVBO);
	glDeleteBuffers(1, &gridEBO);
	glDeleteBuffers(1, &instanceVBO);
}

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
#include "QuadTree.h"
#include <stddef.h>

#define QT_DIVISIONS 10
/* Surface cells + outer boundary skirt (4 edges x QT_DIVISIONS segments x 6
   wall verts), kept in sync with buildGrid. */
#define QT_GRID_VERTS (QT_DIVISIONS * QT_DIVISIONS * 6 + 4 * QT_DIVISIONS * 6)
/* Walk nodes per face, and drawn leaves per walk (every face's nodes could
   in principle all be leaves, so no leaf is ever dropped). */
#define QT_NODE_CAP 8192
#define QT_INSTANCE_CAP (6 * QT_NODE_CAP)

/* Terrain drew at planet radius 30 with a first split distance of 128; keep
   the same screen-space behaviour by scaling the start distance with radius. */
#define QT_LEVEL_PER_RADIUS (128.0 / 30.0)
#define QT_SPLIT_LOD 256.0
/* Culling margin as a fraction of the body radius: covers the land relief
   (g_planetStyle.landRelief, capped below this by the debug panel), the
   water shell + swell, and the skirts. */
#define QT_CULL_RELIEF 0.05

int g_qtLegacyPath = 0;

/* One drawn leaf (shaders/patch.glsl, attribute locations 4-10). */
typedef struct qtInstance {
	float corner[4][4];   /* xyz: sphere corners (0,0) (1,0) (0,1) (1,1), camera-relative */
	float q0[4];          /* xyz: cube-space corner (0,0); w: cell half-width (cube units) */
	float eu[3];          /* cube-space edge along u */
	float ev[3];          /* cube-space edge along v */
} qtInstance;

typedef struct qtNode {
	int level, x, y;
	dvec3 center;         /* camera-relative sphere point of the node centre */
} qtNode;

static qtInstance instances[QT_INSTANCE_CAP];
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

	/* Patch instance attributes, locations 4-10 (shaders/patch.glsl). */
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

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ---- Walk ------------------------------------------------------------------ */

/* Per-walk culling inputs shared by every face, all camera-relative double
   (the camera is the origin): the body centre and radius, the horizon
   angle, and the six view-frustum planes (ax + by + cz + d >= 0 inside). */
typedef struct qtCull {
	dvec3 center;
	double radius;
	dvec3 camDir;       /* unit direction centre -> camera */
	double horizon;     /* max visible angle from camDir; > PI disables */
	double frustum[6][4];
} qtCull;

/* Conservative visibility of a node's bounding sphere. A node of cube-space
   half-width `size` has every (displaced) vertex within
   radius*(size*sqrt(2) + QT_CULL_RELIEF) of its centre: projecting the cube
   face onto the unit sphere never stretches distances, and the relief only
   moves vertices radially. Culled nodes are neither drawn nor split, so
   their whole subtree drops out. */
static int nodeVisible(const qtCull *c, dvec3 nodeCenter, double size)
{
	double chord = size * 1.41421356237;
	double r = c->radius * (chord + QT_CULL_RELIEF);

	for(int p = 0; p < 6; p++) {
		const double *pl = c->frustum[p];
		if(pl[0]*nodeCenter.x + pl[1]*nodeCenter.y + pl[2]*nodeCenter.z + pl[3] < -r)
			return 0;
	}

	if(c->horizon < M_PI) {
		dvec3 d = dsubvec3(nodeCenter, c->center);
		double len = dlengthvec3(d);
		if(len > 1e-9 * c->radius) {
			double cosA = (d.x*c->camDir.x + d.y*c->camDir.y + d.z*c->camDir.z) / len;
			if(cosA < -1.0) cosA = -1.0;
			if(cosA > 1.0) cosA = 1.0;
			/* Angular half-size of the node on the unit sphere. */
			double nodeAng = 2.0 * asin(fmin(chord * 0.5, 1.0));
			if(acos(cosA) - nodeAng > c->horizon)
				return 0;
		}
	}
	return 1;
}

static void buildCull(qtCull *c, const dmat4 *model, mat4 viewProj, int useHorizon)
{
	const double (*m)[4] = model->m;
	c->center.x = m[0][3]; c->center.y = m[1][3]; c->center.z = m[2][3];
	/* Uniform scale: length of the model's first basis column. */
	c->radius = sqrt(m[0][0]*m[0][0] + m[1][0]*m[1][0] + m[2][0]*m[2][0]);

	/* Horizon: the solid unit sphere (ocean floor / undisplaced body) hides
	   everything past acos(R/d); terrain up to R*(1 + relief) peeks over it
	   by a further acos(1/(1 + relief)). Inside the sphere: no horizon. */
	double d = dlengthvec3(c->center);
	if(useHorizon && d > c->radius) {
		c->camDir = dscalarvec3(c->center, -1.0 / d);
		c->horizon = acos(c->radius/d) + acos(1.0/(1.0 + QT_CULL_RELIEF));
	}
	else {
		c->camDir.x = 0.0; c->camDir.y = 0.0; c->camDir.z = 1.0;
		c->horizon = 4.0;
	}

	/* Gribb-Hartmann planes from clip = viewProj * world (column vectors,
	   rows m[i][*]): left/right, bottom/top, near/far. */
	for(int p = 0; p < 6; p++) {
		int row = p >> 1;
		double sign = (p & 1) ? -1.0 : 1.0;
		for(int k = 0; k < 4; k++)
			c->frustum[p][k] = (double)viewProj.m[3][k] + sign*(double)viewProj.m[row][k];
		double len = sqrt(c->frustum[p][0]*c->frustum[p][0] + c->frustum[p][1]*c->frustum[p][1] + c->frustum[p][2]*c->frustum[p][2]);
		if(len > 0.0)
			for(int k = 0; k < 4; k++)
				c->frustum[p][k] /= len;
	}
}

/* Appends the instance for a leaf: its four sphere corners in double,
   camera-relative, then downcast (shaders/patch.glsl rebuilds the surface
   from them). */
static void emitLeaf(const dmat4 *model, int f, const qtNode *nd)
{
	if(instanceCount >= QT_INSTANCE_CAP)
		return;
	qtInstance *inst = &instances[instanceCount++];
	double size = ldexp(1.0, -nd->level);

	for(int i = 0; i < 4; i++) {
		dvec3 c = spherePoint(model, cubePoint(f, nd->level, nd->x + (i & 1), nd->y + (i >> 1)));
		inst->corner[i][0] = (float)c.x;
		inst->corner[i][1] = (float)c.y;
		inst->corner[i][2] = (float)c.z;
		inst->corner[i][3] = 0.0f;
	}
	dvec3 q0 = cubePoint(f, nd->level, nd->x, nd->y);
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
}

/* One frame's quadtree walk over a single face: appends the visible leaf
   instances (split parents and culled nodes emit nothing). */
static void traverseFace(const dmat4 *model, int f, const qtCull *cull, double level)
{
	static qtNode nodes[QT_NODE_CAP];

	nodes[0].level = 0; nodes[0].x = 0; nodes[0].y = 0;
	nodes[0].center = spherePoint(model, cubePoint(f, 0, 0.5, 0.5));

	int maxLOD = (int)(log(QT_SPLIT_LOD)/log(2.0) + 0.5);
	int numNodes = 1;
	int levelStart = 0;

	for(int depth = 0; depth <= maxLOD; depth++) {
		int levelEnd = numNodes;
		for(int k = levelStart; k < levelEnd; k++) {
			const qtNode *nd = &nodes[k];
			double size = ldexp(1.0, -nd->level);
			if(!nodeVisible(cull, nd->center, size))
				continue;

			/* Camera at the origin: the node's distance is |centre|. */
			int split = depth < maxLOD
			         && numNodes + 4 <= QT_NODE_CAP
			         && dlengthvec3(nd->center) < level;
			if(split) {
				for(int j = 0; j < 4; j++) {
					qtNode *child = &nodes[numNodes++];
					child->level = nd->level + 1;
					child->x = 2*nd->x + (j & 1);
					child->y = 2*nd->y + (j >> 1);
					child->center = spherePoint(model, cubePoint(f, child->level, child->x + 0.5, child->y + 0.5));
				}
				continue;
			}
			emitLeaf(model, f, nd);
		}
		levelStart = levelEnd;
		level *= 0.5;
	}
}

/* Last walk's inputs: the planet and water passes draw the same body from
   the same camera back to back, so the second pass reuses the instance
   buffer the first one uploaded. The camera is always the origin, so a
   moved camera shows up as a changed (camera-relative) model matrix. */
static dmat4 lastModel;
static mat4 lastViewProj;
static double lastRadius = -1.0;

static void walkFaces(const dmat4 *model, double radius, mat4 viewProj, int useHorizon)
{
	qtCull cull;
	buildCull(&cull, model, viewProj, useHorizon);
	double level0 = radius * QT_LEVEL_PER_RADIUS;

	instanceCount = 0;
	for(int f = 0; f < 6; f++)
		traverseFace(model, f, &cull, level0);

	if(instanceCount > 0) {
		/* Full re-specify orphans the old storage, so a draw still reading
		   the previous body's instances never stalls us. */
		glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof(qtInstance)*instanceCount, instances, GL_STREAM_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
}

static void drawPatches(void)
{
	if(instanceCount == 0)
		return;

	/* The debug path toggle is a uniform of whichever body program is bound. */
	GLint program = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glUniform1i(glGetUniformLocation((GLuint)program, "patchLegacy"), g_qtLegacyPath);

	/* Patch skirts must render double-sided, so culling is off entirely for
	   this pass and the depth buffer resolves the near/far hemispheres (the
	   planets pass is opaque). Wireframe therefore also shows the far
	   hemisphere's grid and walls. Hand the normal GL_BACK state back to the
	   rest of the frame afterwards. */
	glDisable(GL_CULL_FACE);
	glBindVertexArray(gridVAO);
	glDrawElementsInstanced(GL_TRIANGLES, QT_GRID_VERTS, GL_UNSIGNED_SHORT, BUFFER_OFFSET(0), instanceCount);
	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
}

void QuadTree_DrawShadowCasters(const dmat4 *model, double radius, mat4 lightViewProj)
{
	/* Same camera-centred LOD as the main pass (so caster and receiver
	   meshes match), culled against the light frustum, and without the
	   camera horizon cull: terrain past the horizon can still shadow what
	   the camera sees. It overwrites the instance buffer, so the next
	   QuadTree_DrawSphere must walk again. */
	walkFaces(model, radius, lightViewProj, 0);
	lastRadius = -1.0;
	drawPatches();
}

void QuadTree_DrawSphere(const dmat4 *model, double radius, mat4 viewProj)
{
	int cached = radius == lastRadius
	          && memcmp(model, &lastModel, sizeof(dmat4)) == 0
	          && memcmp(&viewProj, &lastViewProj, sizeof(mat4)) == 0;

	if(!cached) {
		walkFaces(model, radius, viewProj, 1);
		lastModel = *model; lastViewProj = viewProj; lastRadius = radius;
	}

	drawPatches();
}

void QuadTree_Shutdown(void)
{
	glDeleteVertexArrays(1, &gridVAO);
	glDeleteBuffers(1, &gridVBO);
	glDeleteBuffers(1, &gridEBO);
	glDeleteBuffers(1, &instanceVBO);
}

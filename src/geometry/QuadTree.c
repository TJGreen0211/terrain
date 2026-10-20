/* A cube-face quadtree LOD sphere. Each cube face is a baked 10x10 grid
   lying on its constant-coordinate plane; the vertex shader instance-
   transforms each grid cell and normalizes it onto the unit sphere, so 6
   face grids tile the whole ball. The CPU walk per frame subdivides each
   face's quadtree around the camera: nodes nearer than the (halving) split
   distance are replaced by their 4 children and the parent instance is
   collapsed to a zero matrix, and every remaining node is one instance in a
   single instanced draw per face. Boundary skirt vertices (position w = 1)
   duplicate each face-grid edge slightly below the surface so LOD patch
   edges never crack. */
#include "QuadTree.h"

#define QT_INSTANCE_CAP 8192
#define QT_DIVISIONS 10
/* Surface cells + outer boundary skirt (4 edges x QT_DIVISIONS segments x 6
   wall verts), kept in sync with buildFaceGrid. */
#define QT_FACE_VERTS (QT_DIVISIONS * QT_DIVISIONS * 6 + 4 * QT_DIVISIONS * 6)

/* Terrain drew at planet radius 30 with a first split distance of 128; keep
   the same screen-space behaviour by scaling the start distance with radius. */
#define QT_LEVEL_PER_RADIUS (128.0f / 30.0f)
#define QT_SPLIT_LOD 256.0f
/* Culling margin as a fraction of the body radius: covers the land relief
   (LAND_RELIEF in Main.c), the water shell + swell, and the skirts. */
#define QT_CULL_RELIEF 0.05f

static mat4 pos[QT_INSTANCE_CAP];

static GLuint faceVAO[6];
static GLuint faceVBO[6];
static GLuint instanceVBO[6];

/* Grid axes per order[] from the terrain Main.c call sites. The baked grids
   are NOT on the axis their name suggests: the X grid builds on z=-1, the Y
   grid on y=+1, the Z grid on x=+1. order[] says where the 2D coords
   (values[0], values[1]) and the constant axis (values[2]) land: axis i
   carries values[order[i]]. So n = axis holding values[2], a = axis holding
   values[0], b = axis holding values[1], and plane is the sign of the baked
   constant coordinate. */
static const int faceOrder[6][3] = {
	{0, 1, 2}, {0, 1, 2},   /* X grids: built on z=-1 */
	{0, 2, 1}, {0, 2, 1},   /* Y grids: built on y=+1 */
	{2, 0, 1}, {2, 0, 1}    /* Z grids: built on x=+1 */
};
/* Front faces draw as built; back faces are mirrored (negate flips the
   constant axis) and baked with reversed winding so both stay outward. */
static const float faceNegate[6] = { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
static const int faceGrid[6] = { 0, 0, 1, 1, 2, 2 };   /* 0=X 1=Y 2=Z */
static const int faceReverse[6] = { 0, 1, 0, 1, 0, 1 };

static void faceAxes(const int *order, int *n, int *a, int *b, float *plane) {
	for(int i = 0; i < 3; i++) {
		if(order[i] == 2) *n = i;
		else if(order[i] == 0) *a = i;
		else *b = i;
	}
	/* X grid (order {0,1,2}) sits at z=-1; Y ({0,2,1}) at y=+1; Z ({2,0,1}) at x=+1. */
	*plane = (order[0] == 0 && order[1] == 1) ? -1.0f : 1.0f;
}

static void setAxis(vec3 *v, int axis, float value) {
	if(axis == 0) v->x = value;
	else if(axis == 1) v->y = value;
	else v->z = value;
}

/* Keep the CPU matrix layout consistent with the other row-vector transforms:
   OpenGL reads the row-major data as columns, so translate * scale offsets
   the vertex before the shader projects it onto the sphere. */
static mat4 instanceMatrix(vec3 t, vec3 s) {
	return multiplymat4(translatevec3(t), scalevec3(s));
}

static float pointDistance(vec3 u, vec3 v) {
	return sqrtf((v.x - u.x)*(v.x - u.x) + (v.y - u.y)*(v.y - u.y) + (v.z - u.z)*(v.z - u.z));
}

/* World-space point on the rendered sphere for a node: model * normalize(plocal),
   matching quad.vert (instance-transform, normalize to unit sphere, then model).
   Rows written explicitly: multiplymat4vec4 has an in-place clobbering defect. */
static vec3 sphereCenter(mat4 model, vec3 plocal) {
	vec3 pdir = normalizevec3(plocal);
	vec3 c;
	c.x = model.m[0][0]*pdir.x + model.m[0][1]*pdir.y + model.m[0][2]*pdir.z + model.m[0][3];
	c.y = model.m[1][0]*pdir.x + model.m[1][1]*pdir.y + model.m[1][2]*pdir.z + model.m[1][3];
	c.z = model.m[2][0]*pdir.x + model.m[2][1]*pdir.y + model.m[2][2]*pdir.z + model.m[2][3];
	return c;
}

/* Point at grid indices (u, v), each in [0, QT_DIVISIONS], on the grid plane:
   u runs along the inner-scroll axis, v along the outer one, using the same
   start/offset rules as the cell loop below. */
static vec4 boundaryPoint(int grid, int u, int v) {
	float offset = 2.0f / (float)QT_DIVISIONS;
	float su = 1.0f - offset*(float)u;
	float sv = 1.0f - offset*(float)v;
	vec4 p;
	if(grid == 0) { p.x = su; p.y = sv; p.z = -1.0f; }
	else if(grid == 1) { p.x = su; p.y = 1.0f; p.z = sv; }
	else { p.x = 1.0f; p.y = sv; p.z = su; }
	p.w = 0.0f;
	return p;
}

/* Face grid vertices (port of the terrain initSubQuadX/Y/Z builders, minus
   the texture/tangent streams the planet shader does not consume). Positions
   are vec4: w is a skirt flag (0 = surface, 1 = wall) that planet.vert uses to
   push duplicated boundary vertices below the surface, hiding height
   mismatches along patch edges. */
static int buildFaceGrid(int grid, int reverseOrder, vec4 *out) {
	int order[6];
	if(reverseOrder) {
		order[0] = 2; order[1] = 1; order[2] = 0; order[3] = 1; order[4] = 3; order[5] = 0;
	}
	else {
		order[0] = 0; order[1] = 1; order[2] = 2; order[3] = 0; order[4] = 3; order[5] = 1;
	}

	vec3 start;
	if(grid == 0) { start.x = 1.0f; start.y = 1.0f; start.z = -1.0f; }  /* X grid on z=-1 */
	else if(grid == 1) { start.x = 1.0f; start.y = 1.0f; start.z = 1.0f; }  /* Y grid on y=+1 */
	else { start.x = 1.0f; start.y = 1.0f; start.z = 1.0f; }               /* Z grid on x=+1 */

	float offset = 2.0f / (float)QT_DIVISIONS;
	int index = 0;
	for(int i = 0; i < QT_DIVISIONS; i++) {
		if(grid == 0) start.x = 1.0f;
		else if(grid == 1) start.x = 1.0f;
		else start.z = 1.0f;
		for(int j = 0; j < QT_DIVISIONS; j++) {
			vec3 face[4];
			if(grid == 0) {
				face[0] = start;
				face[1].x = start.x-offset; face[1].y = start.y-offset; face[1].z = start.z;
				face[2].x = start.x; face[2].y = start.y-offset; face[2].z = start.z;
				face[3].x = start.x-offset; face[3].y = start.y; face[3].z = start.z;
			}
			else if(grid == 1) {
				face[0] = start;
				face[1].x = start.x-offset; face[1].y = start.y; face[1].z = start.z-offset;
				face[2].x = start.x; face[2].y = start.y; face[2].z = start.z-offset;
				face[3].x = start.x-offset; face[3].y = start.y; face[3].z = start.z;
			}
			else {
				face[0] = start;
				face[1].x = start.x; face[1].y = start.y-offset; face[1].z = start.z-offset;
				face[2].x = start.x; face[2].y = start.y-offset; face[2].z = start.z;
				face[3].x = start.x; face[3].y = start.y; face[3].z = start.z-offset;
			}
			for(int k = 0; k < 6; k++) {
				vec3 v = face[order[k]];
				out[index].x = v.x; out[index].y = v.y; out[index].z = v.z;
				out[index].w = 0.0f;
				index++;
			}

			/* Inner loop scrolls the columns; outer loop (below) the rows. */
			if(grid == 2) start.z -= offset;
			else start.x -= offset;
		}
		if(grid == 1) start.z -= offset;
		else start.y -= offset;
	}

	/* Boundary skirt: walk the grid perimeter once and duplicate each boundary
	   point as a wall vertex (same plane coords, w = 1); the vertex shader
	   pulls the wall ring radially below the lowest possible displaced
	   surface, so the wall hides T-junction and mip mismatches at every patch
	   edge. Culling is off in this pass, so the winding is irrelevant and the
	   same order serves both front/back grids. */
	static const int edgeStart[4][2] = {
		{0, 0}, {QT_DIVISIONS, 0}, {QT_DIVISIONS, QT_DIVISIONS}, {0, QT_DIVISIONS}
	};
	static const int edgeStep[4][2] = {
		{1, 0}, {0, 1}, {-1, 0}, {0, -1}
	};
	for(int e = 0; e < 4; e++) {
		for(int s = 0; s < QT_DIVISIONS; s++) {
			vec4 b0 = boundaryPoint(grid, edgeStart[e][0] + edgeStep[e][0]*s,
			                         edgeStart[e][1] + edgeStep[e][1]*s);
			vec4 b1 = boundaryPoint(grid, edgeStart[e][0] + edgeStep[e][0]*(s+1),
			                         edgeStart[e][1] + edgeStep[e][1]*(s+1));
			vec4 s0 = b0; s0.w = 1.0f;
			vec4 s1 = b1; s1.w = 1.0f;
			out[index++] = b0;
			out[index++] = b1;
			out[index++] = s0;
			out[index++] = s0;
			out[index++] = b1;
			out[index++] = s1;
		}
	}
	return index;
}

void QuadTree_InitGL(GLuint shader)
{
	GLuint vPosition = glGetAttribLocation(shader, "vPosition");

	/* One instance buffer per face, so a cached walk (QuadTree_DrawSphere)
	   can be redrawn without re-uploading. */
	glGenBuffers(6, instanceVBO);

	for(int f = 0; f < 6; f++) {
		vec4 verts[QT_FACE_VERTS];
		buildFaceGrid(faceGrid[f], faceReverse[f], verts);

		glGenVertexArrays(1, &faceVAO[f]);
		glBindVertexArray(faceVAO[f]);

		glGenBuffers(1, &faceVBO[f]);
		glBindBuffer(GL_ARRAY_BUFFER, faceVBO[f]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
		glEnableVertexAttribArray(vPosition);
		/* vec4 positions: w carries the skirt flag (moon's planetVAO keeps a
		   3-float attribute, where w defaults to 1 but is never read). */
		glVertexAttribPointer(vPosition, 4, GL_FLOAT, GL_FALSE, 4*sizeof(float), BUFFER_OFFSET(0));

		/* mat4 instance transform in attributes 4..7 (quad.vert layout). */
		glBindBuffer(GL_ARRAY_BUFFER, instanceVBO[f]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(mat4)*QT_INSTANCE_CAP, NULL, GL_STREAM_DRAW);
		for(int k = 0; k < 4; k++) {
			glEnableVertexAttribArray(4+k);
			glVertexAttribPointer(4+k, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(k*sizeof(vec4)));
			glVertexAttribDivisor(4+k, 1);
		}
		glBindVertexArray(0);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* Per-walk culling inputs shared by every face: the body's world centre and
   radius, the world camera, the horizon angle, and the six view-frustum
   planes (ax + by + cz + d >= 0 inside). */
typedef struct qtCull {
	vec3 center;
	float radius;
	vec3 camPos;
	vec3 camDir;        /* unit direction centre -> camera */
	float horizon;      /* max visible angle from camDir; > PI disables */
	float frustum[6][4];
} qtCull;

/* Conservative visibility of a node's bounding sphere. A node of face
   half-width qScale has every (displaced) vertex within
   radius*(qScale*sqrt(2) + QT_CULL_RELIEF) of its centre: projecting the
   cube face onto the unit sphere never stretches distances, and the relief
   only moves vertices radially. Culled nodes are neither drawn nor split,
   so their whole subtree drops out. */
static int nodeVisible(const qtCull *c, vec3 nodeCenter, float qScale)
{
	float chord = qScale * 1.41421356f;
	float r = c->radius * (chord + QT_CULL_RELIEF);

	for(int p = 0; p < 6; p++) {
		const float *pl = c->frustum[p];
		if(pl[0]*nodeCenter.x + pl[1]*nodeCenter.y + pl[2]*nodeCenter.z + pl[3] < -r)
			return 0;
	}

	if(c->horizon < (float)M_PI) {
		vec3 d = { nodeCenter.x - c->center.x, nodeCenter.y - c->center.y, nodeCenter.z - c->center.z };
		float len = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
		if(len > 1e-6f) {
			float cosA = (d.x*c->camDir.x + d.y*c->camDir.y + d.z*c->camDir.z) / len;
			if(cosA < -1.0f) cosA = -1.0f;
			if(cosA > 1.0f) cosA = 1.0f;
			/* Angular half-size of the node on the unit sphere. */
			float nodeAng = 2.0f * asinf(fminf(chord * 0.5f, 1.0f));
			if(acosf(cosA) - nodeAng > c->horizon)
				return 0;
		}
	}
	return 1;
}

/* One frame's quadtree walk over a single face: fills out[] with the visible
   leaf instances only (split parents and culled nodes emit nothing) and
   returns the instance count. Port of the terrain traverseQuad traversal. */
static int traverseFace(mat4 model, const int *order, float negate, const qtCull *cull, float level, mat4 *out)
{
	static qTreeNode qq[QT_INSTANCE_CAP];
	static float qqScale[QT_INSTANCE_CAP];

	int n, a, b;
	float plane;
	faceAxes(order, &n, &a, &b, &plane);

	/* Root node: the full face. nw stores the node's 2D center on the face;
	   its LOD point is the face center pushed onto the rendered sphere. */
	qq[0].nw.x = 0.0; qq[0].nw.y = 0.0; qq[0].nw.z = 0.0;
	qq[0].width = 2.0;
	qqScale[0] = 1.0f;
	vec3 rootLocal = {0.0, 0.0, 0.0};
	setAxis(&rootLocal, n, negate*plane);
	qq[0].center = sphereCenter(model, rootLocal);

	int terrainMaxLOD = (int)(log(QT_SPLIT_LOD)/log(2.0));
	int numNodes = 1;
	int levelStart = 0;
	int emitted = 0;

	for(int depth = 0; depth <= terrainMaxLOD; depth++) {
		int levelEnd = numNodes;
		for(int k = levelStart; k < levelEnd; k++) {
			float qScale = qqScale[k];
			if(!nodeVisible(cull, qq[k].center, qScale))
				continue;

			int split = depth < terrainMaxLOD
			         && numNodes + 4 <= QT_INSTANCE_CAP
			         && pointDistance(qq[k].center, cull->camPos) < level;
			if(split) {
				float cs = qScale * 0.5f;
				for(int j = 0; j < 4; j++) {
					/* Child 2D centers: (+,+), (-,+), (-,-), (+,-) around the parent. */
					float ox = (j == 0 || j == 3) ? cs : -cs;
					float oy = (j == 0 || j == 1) ? cs : -cs;
					float cx = qq[k].nw.x + ox;
					float cy = qq[k].nw.y + oy;

					qq[numNodes].nw.x = cx; qq[numNodes].nw.y = cy; qq[numNodes].nw.z = 0.0;
					qq[numNodes].width = cs*2.0f;
					qqScale[numNodes] = cs;

					/* LOD point: the child's center on the face, projected to the sphere. */
					vec3 plocal = {0.0, 0.0, 0.0};
					setAxis(&plocal, a, cx); setAxis(&plocal, b, cy); setAxis(&plocal, n, negate*plane);
					qq[numNodes].center = sphereCenter(model, plocal);
					numNodes++;
				}
				continue;
			}

			/* Leaf instance: shrink in-plane axes to the node size, keep the
			   face-normal axis at +/-1, shift the node onto its quadrant. */
			vec3 t = {0.0, 0.0, 0.0};
			setAxis(&t, a, qq[k].nw.x); setAxis(&t, b, qq[k].nw.y);
			vec3 s = {qScale, qScale, qScale};
			setAxis(&s, n, negate);
			out[emitted] = instanceMatrix(t, s);
			/* Spare matrix slot (m[3][0], identity row-3 otherwise carries no
			   data): this instance's face cell half-width. The vertex shader
			   derives the height-map mip from it. */
			out[emitted].m[3][0] = qScale;
			emitted++;
		}
		levelStart = levelEnd;
		level /= 2.0f;
	}
	return emitted;
}

static void buildCull(qtCull *c, mat4 model, mat4 viewProj, vec3 camPos)
{
	c->center.x = model.m[0][3]; c->center.y = model.m[1][3]; c->center.z = model.m[2][3];
	/* Uniform scale: length of the model's first basis column. */
	c->radius = sqrtf(model.m[0][0]*model.m[0][0] + model.m[1][0]*model.m[1][0] + model.m[2][0]*model.m[2][0]);
	c->camPos = camPos;

	/* Horizon: the solid unit sphere (ocean floor / undisplaced body) hides
	   everything past acos(R/d); terrain up to R*(1 + relief) peeks over it
	   by a further acos(1/(1 + relief)). Inside the sphere: no horizon. */
	vec3 rel = { camPos.x - c->center.x, camPos.y - c->center.y, camPos.z - c->center.z };
	float d = sqrtf(rel.x*rel.x + rel.y*rel.y + rel.z*rel.z);
	if(d > c->radius) {
		c->camDir.x = rel.x/d; c->camDir.y = rel.y/d; c->camDir.z = rel.z/d;
		c->horizon = acosf(c->radius/d) + acosf(1.0f/(1.0f + QT_CULL_RELIEF));
	}
	else {
		c->camDir.x = 0.0f; c->camDir.y = 0.0f; c->camDir.z = 1.0f;
		c->horizon = 4.0f;
	}

	/* Gribb-Hartmann planes from clip = viewProj * world (column vectors,
	   rows m[i][*]): left/right, bottom/top, near/far. */
	for(int p = 0; p < 6; p++) {
		int row = p >> 1;
		float sign = (p & 1) ? -1.0f : 1.0f;
		float len;
		for(int k = 0; k < 4; k++)
			c->frustum[p][k] = viewProj.m[3][k] + sign*viewProj.m[row][k];
		len = sqrtf(c->frustum[p][0]*c->frustum[p][0] + c->frustum[p][1]*c->frustum[p][1] + c->frustum[p][2]*c->frustum[p][2]);
		if(len > 0.0f)
			for(int k = 0; k < 4; k++)
				c->frustum[p][k] /= len;
	}
}

void QuadTree_DrawSphere(mat4 model, float radius, mat4 viewProj)
{
	/* Last walk's inputs: the planet and water passes draw the same body
	   from the same camera back to back, so the second pass reuses the
	   instance buffers the first one uploaded. */
	static mat4 lastModel, lastViewProj;
	static vec3 lastCam;
	static float lastRadius = -1.0f;
	static int faceCount[6];

	/* Terrain keeps absolute world coordinates (no floating origin): the
	   node centres computed by sphereCenter are world-space, so the LOD walk
	   compares them against the world camera position, not (0,0,0). */
	vec4 cam = getCameraPosition(identityMatrix());
	vec3 camPos = { cam.x, cam.y, cam.z };

	int cached = radius == lastRadius
	          && camPos.x == lastCam.x && camPos.y == lastCam.y && camPos.z == lastCam.z
	          && memcmp(&model, &lastModel, sizeof(mat4)) == 0
	          && memcmp(&viewProj, &lastViewProj, sizeof(mat4)) == 0;

	if(!cached) {
		qtCull cull;
		buildCull(&cull, model, viewProj, camPos);
		float level0 = radius * QT_LEVEL_PER_RADIUS;
		for(int f = 0; f < 6; f++) {
			faceCount[f] = traverseFace(model, faceOrder[f], faceNegate[f], &cull, level0, pos);
			if(faceCount[f] > 0) {
				/* Full re-specify orphans the old storage, so a draw still
				   reading the previous body's instances never stalls us. */
				glBindBuffer(GL_ARRAY_BUFFER, instanceVBO[f]);
				glBufferData(GL_ARRAY_BUFFER, sizeof(mat4)*faceCount[f], pos, GL_STREAM_DRAW);
			}
		}
		lastModel = model; lastViewProj = viewProj; lastCam = camPos; lastRadius = radius;
	}

	/* Patch skirts must render double-sided, so culling is off entirely for
	   this pass and the depth buffer resolves the near/far hemispheres (the
	   planets pass is opaque). Wireframe therefore also shows the far
	   hemisphere's grid and walls. Hand the normal GL_BACK state back to the
	   rest of the frame afterwards. */
	glDisable(GL_CULL_FACE);

	for(int f = 0; f < 6; f++) {
		if(faceCount[f] == 0)
			continue;
		glBindVertexArray(faceVAO[f]);
		glDrawArraysInstanced(GL_TRIANGLES, 0, QT_FACE_VERTS, faceCount[f]);
	}
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
}

void QuadTree_Shutdown(void)
{
	for(int f = 0; f < 6; f++) {
		glDeleteVertexArrays(1, &faceVAO[f]);
		glDeleteBuffers(1, &faceVBO[f]);
	}
	glDeleteBuffers(6, instanceVBO);
}

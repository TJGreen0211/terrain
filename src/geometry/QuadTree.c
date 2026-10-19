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

static mat4 pos[QT_INSTANCE_CAP];

static GLuint faceVAO[6];
static GLuint faceVBO[6];
static GLuint instanceVBO;

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

	glGenBuffers(1, &instanceVBO);
	glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(mat4)*QT_INSTANCE_CAP, NULL, GL_DYNAMIC_DRAW);

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
		glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
		for(int k = 0; k < 4; k++) {
			glEnableVertexAttribArray(4+k);
			glVertexAttribPointer(4+k, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(k*sizeof(vec4)));
			glVertexAttribDivisor(4+k, 1);
		}
		glBindVertexArray(0);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* One frame's quadtree walk over a single face: fills pos[] with the visible
   node instances (zeroed parents + leaves) and returns the instance count.
   Port of the terrain traverseQuad traversal. */
static int traverseFace(mat4 model, const int *order, float negate, vec3 camPos, float level)
{
	int globalCount = 0;
	float qScale = 1.0;

	int n, a, b;
	float plane;
	faceAxes(order, &n, &a, &b, &plane);

	qTreeNode qq[QT_INSTANCE_CAP];

	/* Root node: the full face. nw stores the node's 2D center on the face;
	   its LOD point is the face center pushed onto the rendered sphere. */
	qq[0].nw.x = 0.0; qq[0].nw.y = 0.0; qq[0].nw.z = 0.0;
	qq[0].width = 2.0;
	vec3 rootLocal = {0.0, 0.0, 0.0};
	setAxis(&rootLocal, n, negate*plane);
	qq[0].center = sphereCenter(model, rootLocal);

	vec3 rootT = {0.0, 0.0, 0.0};
	vec3 rootS = {1.0, 1.0, 1.0};
	setAxis(&rootS, n, negate);
	pos[globalCount] = instanceMatrix(rootT, rootS);
	/* Spare matrix slot (m[3][0], identity row-3 otherwise carries no data):
	   this instance's face cell half-width. The vertex shader derives the
	   height-map mip from it; sphereCenter only reads rows 0..2. */
	pos[globalCount].m[3][0] = 1.0f;

	int numNodes = 1;
	int terrainMaxLOD = (int)(log(QT_SPLIT_LOD)/log(2.0));
	int lastNodeCount = 0;

	for(int i = 0; i < terrainMaxLOD; i++) {
		qScale /= 2.0;

		for(int k = lastNodeCount; k < numNodes; k++) {
			if(pointDistance(qq[k].center, camPos) < level) {
				if(globalCount + 4 >= QT_INSTANCE_CAP)
					break;
				pos[k] = scale(0.0);
				for(int j = 0; j < 4; j++) {
					globalCount++;

					/* Child 2D centers: (+,+), (-,+), (-,-), (+,-) around the parent. */
					float ox = (j == 0 || j == 3) ? qScale : -qScale;
					float oy = (j == 0 || j == 1) ? qScale : -qScale;
					float cx = qq[k].nw.x + ox;
					float cy = qq[k].nw.y + oy;

					/* Instance: shrink in-plane axes to the child size, keep the
					   face-normal axis at +/-1, shift the child onto its quadrant. */
					vec3 t = {0.0, 0.0, 0.0};
					setAxis(&t, a, cx); setAxis(&t, b, cy);
					vec3 s = {qScale, qScale, qScale};
					setAxis(&s, n, negate);
					pos[globalCount] = instanceMatrix(t, s);
					pos[globalCount].m[3][0] = qScale;

					qq[globalCount].nw.x = cx; qq[globalCount].nw.y = cy; qq[globalCount].nw.z = 0.0;
					qq[globalCount].width = qScale*2.0f;

					/* LOD point: the child's center on the face, projected to the sphere. */
					vec3 plocal = {0.0, 0.0, 0.0};
					setAxis(&plocal, a, cx); setAxis(&plocal, b, cy); setAxis(&plocal, n, negate*plane);
					qq[globalCount].center = sphereCenter(model, plocal);
				}
			}
		}
		lastNodeCount = numNodes;
		numNodes = globalCount+1;
		level /= 2.0f;
	}
	return globalCount + 1;
}

void QuadTree_DrawSphere(mat4 model, float radius)
{
	/* Terrain keeps absolute world coordinates (no floating origin): the
	   node centres computed by sphereCenter are world-space, so the LOD walk
	   compares them against the world camera position, not (0,0,0). */
	vec4 cam = getCameraPosition(identityMatrix());
	vec3 camPos = { cam.x, cam.y, cam.z };
	float level0 = radius * QT_LEVEL_PER_RADIUS;

	/* Patch skirts must render double-sided, so culling is off entirely for
	   this pass and the depth buffer resolves the near/far hemispheres (the
	   planets pass is opaque). Wireframe therefore also shows the far
	   hemisphere's grid and walls. Hand the normal GL_BACK state back to the
	   rest of the frame afterwards. */
	glDisable(GL_CULL_FACE);

	for(int f = 0; f < 6; f++) {
		int count = traverseFace(model, faceOrder[f], faceNegate[f], camPos, level0);

		glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(mat4)*count, pos);

		glBindVertexArray(faceVAO[f]);
		glDrawArraysInstanced(GL_TRIANGLES, 0, QT_FACE_VERTS, count);
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
	glDeleteBuffers(1, &instanceVBO);
}

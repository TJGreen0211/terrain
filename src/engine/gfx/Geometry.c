#include "engine/gfx/Geometry.h"

#define degToRad (M_PI / 180.0)


vec3 *Mesh_Tangents(int vertexNumber, vec3 *points, vec3 *tangent)
{
	vec3 edge1, edge2, deltaUV1, deltaUV2;

	for(int i = 0; i < vertexNumber; i+=3)
	{
		edge1.x = points[i+1].x - points[i].x;
		edge1.y = points[i+1].y - points[i].y;
		edge1.z = points[i+1].z - points[i].z;
		edge2.x = points[i+2].x - points[i].x;
		edge2.y = points[i+2].y - points[i].y;
		edge2.z = points[i+2].z - points[i].z;

		deltaUV1.x = points[i+1].x - points[i].x;
		deltaUV1.y = points[i+1].y - points[i].y;
		deltaUV2.x = points[i+2].x - points[i].x;
		deltaUV2.y = points[i+2].y - points[i].y;

		//float fDiv = (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y)
		float f = 1.0 / (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
		tangent[i].x = f  * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
		tangent[i].y = f  * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
		tangent[i].z = f  * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
		tangent[i] = normalizevec3(tangent[i]);
		for(int j = i; j < i+3; j++)
			tangent[j] = tangent[i];
	}
	return tangent;
}

vec3 *Mesh_SmoothNormals(vec3 vna[], vec3 *vertices, vec3 *normals, int size) {
	vec3 vn;
	for(int i = 0; i < size; i++) {
		vec3 tempvn = {0.0, 0.0, 0.0};
		vn = vertices[i];
		for(int j = 0; j < size; j++) {
			if(vn.x == vertices[j].x && vn.y == vertices[j].y && vn.z == vertices[j].z) {
				tempvn = plusequalvec3(tempvn, normals[j]);
			}
		}
		vna[i] = normalizevec3(tempvn);
	}
	return vna;
}

vec3 *Mesh_Normals(vec3 normals[], float *vertices, int size) {
	vec3 one, two;
	int c = 0;
	for(int i = 0; i < size; i+=9) {
		one.x = vertices[i+3] - vertices[i];
		one.y = vertices[i+4] - vertices[i+1];
		one.z = vertices[i+5] - vertices[i+2];

		two.x = vertices[i+6] - vertices[i+3];
		two.y = vertices[i+7] - vertices[i+4];
		two.z = vertices[i+8] - vertices[i+5];
		vec3 normal = normalizevec3(crossvec3(one, two));
		normals[c] = normal; c++;
		normals[c] = normal; c++;
		normals[c] = normal; c++;
	}

	return normals;
}



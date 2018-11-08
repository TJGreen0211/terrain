#include "Geometry.h"

#define degToRad (M_PI / 180.0)

ring createRing(int numDivides, float innerRad, float outerRad) {
	ring newRing;
	int index = 0;

	newRing.vertexNumber = 360*6;
	newRing.size = newRing.vertexNumber*sizeof(vec3);
	newRing.nsize = newRing.vertexNumber*sizeof(vec3);
	newRing.texsize = newRing.vertexNumber*sizeof(vec2);
	newRing.points = malloc(newRing.size);
	newRing.normals = malloc(newRing.nsize);
	newRing.texCoords = malloc(newRing.texsize);
	vec3 one, two, normal;

	for(int i = 0; i < 360; i++) {
		float deg = i * degToRad;
		newRing.points[index].x = cos(deg)*outerRad;
		newRing.points[index].y = sin(deg)*outerRad;
		newRing.points[index].z = 0.0;
		newRing.texCoords[index].x = 1.0;
		newRing.texCoords[index].y = atan(newRing.points[index].y/newRing.points[index].x);

		newRing.points[index+1].x = cos(deg)*innerRad;
		newRing.points[index+1].y = sin(deg)*innerRad;
		newRing.points[index+1].z = 0.0;
		newRing.texCoords[index+1].x = 0.0;
		newRing.texCoords[index+1].y = atan(newRing.points[index+1].y/newRing.points[index+1].x);

		newRing.points[index+2].x = cos((i+1) * degToRad)*outerRad;
		newRing.points[index+2].y = sin((i+1) * degToRad)*outerRad;
		newRing.points[index+2].z = 0.0;
		newRing.texCoords[index+2].x = 1.0;
		newRing.texCoords[index+2].y = atan(newRing.points[index+2].y/newRing.points[index+2].x);

		one.x = newRing.points[index+1].x - newRing.points[index].x;
		one.y = newRing.points[index+1].y - newRing.points[index].y;
		one.z = newRing.points[index+1].z - newRing.points[index].z;

		two.x = newRing.points[index+2].x - newRing.points[index+1].x;
		two.y = newRing.points[index+2].y - newRing.points[index+1].y;
		two.z = newRing.points[index+2].z - newRing.points[index+1].z;

		normal = normalizevec3(crossvec3(one, two));

		newRing.normals[index] = normal;
		newRing.normals[index+1] = normal;
		newRing.normals[index+2] = normal;

		newRing.points[index+3].x = cos((i+1) * degToRad)*outerRad;
		newRing.points[index+3].y = sin((i+1) * degToRad)*outerRad;
		newRing.points[index+3].z = 0.0;
		newRing.texCoords[index+3].x = 1.0;
		newRing.texCoords[index+3].y = atan(newRing.points[index+3].y/newRing.points[index+3].x);

		newRing.points[index+4].x = cos((i) * degToRad)*innerRad;
		newRing.points[index+4].y = sin((i) * degToRad)*innerRad;
		newRing.points[index+4].z = 0.0;
		newRing.texCoords[index+4].x = 0.0;
		newRing.texCoords[index+4].y = atan(newRing.points[index+4].y/newRing.points[index+4].x);

		newRing.points[index+5].x = cos((i+1) * degToRad)*innerRad;
		newRing.points[index+5].y = sin((i+1) * degToRad)*innerRad;
		newRing.points[index+5].z = 0.0;
		newRing.texCoords[index+5].x = 0.0;
		newRing.texCoords[index+5].y = atan(newRing.points[index+5].y/newRing.points[index+5].x);

		one.x = newRing.points[index+1].x - newRing.points[index].x;
		one.y = newRing.points[index+1].y - newRing.points[index].y;
		one.z = newRing.points[index+1].z - newRing.points[index].z;

		two.x = newRing.points[index+2].x - newRing.points[index+1].x;
		two.y = newRing.points[index+2].y - newRing.points[index+1].y;
		two.z = newRing.points[index+2].z - newRing.points[index+1].z;

		normal = normalizevec3(crossvec3(one, two));

		newRing.normals[index+3] = normal;
		newRing.normals[index+4] = normal;
		newRing.normals[index+5] = normal;

		//printf("inner: %f, %f, %f\n", newRing.points[index].x, newRing.points[index].y, newRing.points[index].z);
		//printf("outer: %f, %f, %f\n", newRing.points[index+1].x, newRing.points[index+1].y, newRing.points[index+1].z);

		index += 6;
	}

	return newRing;
}


vec3 *generateTangents(int vertexNumber, vec3 *points, vec3 *tangent)
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

vec3 *generateSmoothNormals(vec3 vna[], vec3 *vertices, vec3 *normals, int size) {
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

vec3 *generateNormals(vec3 normals[], float *vertices, int size) {
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

unsigned int initSubQuadX(int divisions, int reverseOrder) {
	float fdivisions = (float)divisions;

	vec3 face[4];
	int order[6];
	if(reverseOrder) {
		order[0] = 2; order[1] = 1; order[2] = 0; order[3] = 1; order[4] = 3; order[5] = 0;
	}
	else {
		order[0] = 0; order[1] = 1; order[2] = 2; order[3] = 0; order[4] = 3; order[5] = 1;
	}

	unsigned int vao;
	vec3 *vertices = malloc(divisions*divisions*6*sizeof(vec3));
	vec2 *texCoords = malloc(divisions*divisions*6*sizeof(vec2));

	vec3 start = {1.0, 1.0, -1.0};
	float offset = 2.0/(fdivisions);
	int index = 0;
	for(int i = 0; i < divisions; i++) {
		start.x = 1.0;
		for(int j = 0; j < divisions; j++) {
			face[0].x = start.x; face[0].y = start.y; face[0].z = start.z;
			face[1].x = start.x-offset; face[1].y = start.y-offset; face[1].z = start.z;
			face[2].x = start.x; face[2].y = start.y-offset; face[2].z = start.z;
			face[3].x = start.x-offset; face[3].y = start.y; face[3].z = start.z;

			vertices[index++] = face[order[0]];
			texCoords[index-1].x = (face[order[0]].x+1.0);
			texCoords[index-1].y = (face[order[0]].y+1.0);
			vertices[index++] = face[order[1]];
			texCoords[index-1].x = (face[order[1]].x+1.0);
			texCoords[index-1].y = (face[order[1]].y+1.0);
			vertices[index++] = face[order[2]];
			texCoords[index-1].x = (face[order[2]].x+1.0);
			texCoords[index-1].y = (face[order[2]].y+1.0);

			vertices[index++] = face[order[3]];
			texCoords[index-1].x = (face[order[3]].x+1.0);
			texCoords[index-1].y = (face[order[3]].y+1.0);
			vertices[index++] = face[order[4]];
			texCoords[index-1].x = (face[order[4]].x+1.0);
			texCoords[index-1].y = (face[order[4]].y+1.0);
			vertices[index++] = face[order[5]];
			texCoords[index-1].x = (face[order[5]].x+1.0);
			texCoords[index-1].y = (face[order[5]].y+1.0);

			start.x = start.x - offset;
		}
		start.y -= offset;
	}
	

    //*normArray = *generateNormals(normArray, vertices, numVertices);
	vec3 *tangent = malloc(index*sizeof(vec3));
	//printf("sizeof(vertices): %lu\n\n", index);
	vec3 *vertNorms = malloc(index*sizeof(vec3));
	for(int i = 0; i < index; i++) {
		vertNorms[i] = normalizevec3(vertices[i]);
	}
	*tangent = *generateTangents(index, vertNorms, tangent);
    //vao = initBuffers(vertices, sizeof(vertices), vertices, sizeof(vertices), texCoords, sizeof(texCoords));
	vao = initBufferTangents(vertices, sizeof(vertices), vertices, sizeof(vertices), tangent, sizeof(vertices), texCoords, sizeof(texCoords));
	free(vertices);
	free(texCoords);
	free(tangent);
	free(vertNorms);
    return vao;
}

unsigned int initSubQuadY(int divisions, int reverseOrder) {
	float fdivisions = (float)divisions;

	vec3 face[4];
	int order[6];
	if(reverseOrder) {
		order[0] = 2; order[1] = 1; order[2] = 0; order[3] = 1; order[4] = 3; order[5] = 0;
	}
	else {
		order[0] = 0; order[1] = 1; order[2] = 2; order[3] = 0; order[4] = 3; order[5] = 1;
	}

	unsigned int vao;
	vec3 *vertices = malloc(divisions*divisions*6*sizeof(vec3));
	vec2 *texCoords = malloc(divisions*divisions*6*sizeof(vec2));

	vec3 start = {1.0, 1.0, 1.0};
	float offset = 2.0/(fdivisions);
	int index = 0;
	for(int i = 0; i < divisions; i++) {
		start.x = 1.0;
		for(int j = 0; j < divisions; j++) {
			face[0].x = start.x; face[0].y = start.y; face[0].z = start.z;
			face[1].x = start.x-offset; face[1].y = start.y; face[1].z = start.z-offset;
			face[2].x = start.x; face[2].y = start.y; face[2].z = start.z-offset;
			face[3].x = start.x-offset; face[3].y = start.y; face[3].z = start.z;

			vertices[index++] = face[order[0]];
			texCoords[index-1].x = (face[order[0]].x+1.0);
			texCoords[index-1].y = (face[order[0]].z+1.0);
			vertices[index++] = face[order[1]];
			texCoords[index-1].x = (face[order[1]].x+1.0);
			texCoords[index-1].y = (face[order[1]].z+1.0);
			vertices[index++] = face[order[2]];
			texCoords[index-1].x = (face[order[2]].x+1.0);
			texCoords[index-1].y = (face[order[2]].z+1.0);

			vertices[index++] = face[order[3]];
			texCoords[index-1].x = (face[order[3]].x+1.0);
			texCoords[index-1].y = (face[order[3]].z+1.0);
			vertices[index++] = face[order[4]];
			texCoords[index-1].x = (face[order[4]].x+1.0);
			texCoords[index-1].y = (face[order[4]].z+1.0);
			vertices[index++] = face[order[5]];
			texCoords[index-1].x = (face[order[5]].x+1.0);
			texCoords[index-1].y = (face[order[5]].z+1.0);

			start.x = start.x - offset;
		}
		start.z -= offset;
	}

	vec3 *tangent = malloc(index*sizeof(vec3));
	vec3 *vertNorms = malloc(index*sizeof(vec3));
	for(int i = 0; i < index; i++) {
		vertNorms[i] = normalizevec3(vertices[i]);
	}
	*tangent = *generateTangents(index, vertNorms, tangent);
	vao = initBufferTangents(vertices, sizeof(vertices), vertices, sizeof(vertices), tangent, sizeof(vertices), texCoords, sizeof(texCoords));
	free(vertices);
	free(texCoords);
	free(tangent);
	free(vertNorms);
	return vao;
}

unsigned int initSubQuadZ(int divisions, int reverseOrder) {
	float fdivisions = (float)divisions;

	vec3 face[4];
	int order[6];
	if(reverseOrder) {
		order[0] = 2; order[1] = 1; order[2] = 0; order[3] = 1; order[4] = 3; order[5] = 0;
	}
	else {
		order[0] = 0; order[1] = 1; order[2] = 2; order[3] = 0; order[4] = 3; order[5] = 1;
	}

	unsigned int vao;
	vec3 *vertices = malloc(divisions*divisions*6*sizeof(vec3));
	vec2 *texCoords = malloc(divisions*divisions*6*sizeof(vec2));

	vec3 start = {1.0, 1.0, 1.0};
	float offset = 2.0/(fdivisions);
	int index = 0;
	for(int i = 0; i < divisions; i++) {
		start.z = 1.0;
		for(int j = 0; j < divisions; j++) {

			face[0].x = start.x; face[0].y = start.y; face[0].z = start.z;
			face[1].x = start.x; face[1].y = start.y-offset; face[1].z = start.z-offset;
			face[2].x = start.x; face[2].y = start.y-offset; face[2].z = start.z;
			face[3].x = start.x; face[3].y = start.y; face[3].z = start.z-offset;

			vertices[index++] = face[order[0]];
			texCoords[index-1].x = (face[order[0]].y+1.0);
			texCoords[index-1].y = (face[order[0]].z+1.0);
			vertices[index++] = face[order[1]];
			texCoords[index-1].x = (face[order[1]].y+1.0);
			texCoords[index-1].y = (face[order[1]].z+1.0);
			vertices[index++] = face[order[2]];
			texCoords[index-1].x = (face[order[2]].y+1.0);
			texCoords[index-1].y = (face[order[2]].z+1.0);

			vertices[index++] = face[order[3]];
			texCoords[index-1].x = (face[order[3]].y+1.0);
			texCoords[index-1].y = (face[order[3]].z+1.0);
			vertices[index++] = face[order[4]];
			texCoords[index-1].x = (face[order[4]].y+1.0);
			texCoords[index-1].y = (face[order[4]].z+1.0);
			vertices[index++] = face[order[5]];
			texCoords[index-1].x = (face[order[5]].y+1.0);
			texCoords[index-1].y = (face[order[5]].z+1.0);

			start.z = start.z - offset;
		}
		start.y -= offset;
	}

	vec3 *tangent = malloc(index*sizeof(vec3));
	vec3 *vertNorms = malloc(index*sizeof(vec3));
	for(int i = 0; i < index; i++) {
		vertNorms[i] = normalizevec3(vertices[i]);
	}
	*tangent = *generateTangents(index, vertNorms, tangent);
	vao = initBufferTangents(vertices, sizeof(vertices), vertices, sizeof(vertices), tangent, sizeof(vertices), texCoords, sizeof(texCoords));
	free(vertices);
	free(texCoords);
	free(tangent);
	free(vertNorms);
    return vao;
}


unsigned int initQuadVAO() {
	unsigned int vao;
	float vertices[] = {
		-1.0f, -1.0f, 1.0f,
        -1.0f,  1.0f, 1.0f,
         1.0f,  1.0f, 1.0f,
         1.0f,  1.0f, 1.0f,
         1.0f, -1.0f, 1.0f,
        -1.0f, -1.0f, 1.0f
	};

	float texCoords[] = {
		0.0f, 0.0f,
    	0.0f, 1.0f,
		1.0f, 1.0f,

        1.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f
	};
	int numVertices = (sizeof(vertices)/sizeof(vertices[0]));
    int numTexCoords = (sizeof(texCoords)/sizeof(texCoords[0]));
	int vecSize = numVertices/3;
	int texSize = numTexCoords/2;

    vec3 *vertArray = malloc(vecSize*sizeof(vec3));
    vec3 *normArray = malloc(vecSize*sizeof(vec3));
    vec2 *texArray = malloc(texSize*sizeof(vec2));
    int c = 0;
    for(int i = 0; i < numVertices; i+=3) {
    	vertArray[c].x = vertices[i];
    	vertArray[c].y = vertices[i+1];
    	vertArray[c].z = vertices[i+2];
    	c++;
    }
    c = 0;
    for(int i = 0; i < numTexCoords; i+=2) {
    	texArray[c].x = texCoords[i];
    	texArray[c].y = texCoords[i+1];
    	c++;
    }
    *normArray = *generateNormals(normArray, vertices, numVertices);
    vec3 *vna = malloc(vecSize*sizeof(vec3));
	*vna = *generateSmoothNormals(vna, vertArray, normArray, vecSize);
    vao = initBuffers(vertArray, sizeof(vertices), vna, sizeof(vertices), texArray, sizeof(texCoords));
	free(vertArray);
	free(normArray);
	free(texArray);
	free(vna);
    return vao;
}

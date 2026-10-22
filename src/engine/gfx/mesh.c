#include "engine/gfx/mesh.h"

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

sphere newSphere;

int Index = 0;
void triangle(vec3 a, vec3 b, vec3 c, sphere *s)
{
	vec3 one, two;
	one.x = b.x - a.x;
	one.y = b.y - a.y;
	one.z = b.z - a.z;
	
	two.x = c.x - b.x;
	two.y = c.y - b.y;
	two.z = c.z - b.z;

	vec3 normal = normalizevec3(crossvec3(one, two));
	newSphere.normals[Index] = normal; newSphere.points[Index] = a; Index++;
	newSphere.normals[Index] = normal; newSphere.points[Index] = b; Index++;
	newSphere.normals[Index] = normal; newSphere.points[Index] = c; Index++;
}

vec3 unitCircle(vec3 p)
{
	float length = p.x*p.x + p.y*p.y + p.z*p.z;
	vec3 t = {0.0, 0.0, 0.0};
	if(length > 0)
	{
		t.x = p.x / sqrt(length);
		t.y = p.y / sqrt(length);
		t.z = p.z / sqrt(length);
	}
	return t;
}

void divideTriangle(vec3 a, vec3 b, vec3 c, int count, sphere *s)
{
	if(count > 0) {
		vec3 v1 = unitCircle(addvec3(a, b));
		vec3 v2 = unitCircle(addvec3(a, c));
		vec3 v3 = unitCircle(addvec3(b, c));
		divideTriangle(  a, v1, v2, count - 1, s);
        divideTriangle(  c, v2, v3, count - 1, s);
        divideTriangle(  b, v3, v1, count - 1, s);
        divideTriangle( v1, v3, v2, count - 1, s);
	}
	else {
		triangle(a, b, c, s);
	}
}

sphere Mesh_Tetrahedron(int count, sphere *s)
{
	int numTriangles = pow(4, count+1);
	int numVertices = 3 * numTriangles;
	
	int mallocVertSize = numVertices*sizeof(vec3);
	int mallocNormSize = numVertices*sizeof(vec3);
	
	newSphere.points = malloc(mallocVertSize);
	newSphere.normals = malloc(mallocNormSize);
	
	vec3 v[4] = {
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 0.942809, -0.333333},
		{ -0.816497, -0.471405, -0.333333 },
		{ 0.816497, -0.471405, -0.333333 }
    };
    
    divideTriangle( v[0], v[1], v[2], count, s);
    divideTriangle( v[3], v[2], v[1], count, s);
    divideTriangle( v[0], v[3], v[1], count, s);
    divideTriangle( v[0], v[2], v[3], count, s);
    
    newSphere.vertexNumber = numVertices;
    newSphere.size = mallocVertSize;
    newSphere.nsize = mallocNormSize;
	return newSphere;
}

obj Mesh_LoadObj(char *fname);
obj newObj;

obj Mesh_LoadObj(char *fname)
{	
	FILE *fp;
	fp = fopen(fname, "r");
	
	char ch;
	int read;
	float x, y, z;
	int ix, iy, iz;
	int vertCount = 0;
   	int faceCount = 0;
   	int objectCount = 0;
   	int i = 0;
	int j = 0;
	
	if(!fp)
   	{
      	printf("Error while opening the file.\n");
      	exit(1);
   	}
   	
   	
   	
	while((ch = fgetc(fp)) != EOF)
	{
		read = fscanf(fp, "%c",&ch);
		if(read == 1 && strncmp(&ch, "o ", 1) == 0)
		{
			objectCount+=1;
			//printf("OBJECT COUNT: %d\n", objectCount);
		}
		if(read == 1 && ch == 'v')
		{
			fscanf(fp, "%f", &x);
			fscanf(fp, "%f", &y);
			fscanf(fp, "%f", &z);
			vertCount++; 
		}
		if(read == 1 && ch == 'f')
		{
			fscanf(fp, "%d", &ix);
			fscanf(fp, "%d", &iy);
			fscanf(fp, "%d", &iz);
			faceCount++;
		}
	}
	
	rewind(fp);
	
	vec3 *verts = malloc(vertCount*sizeof(vec3));
	vec3 *faces = malloc(faceCount*sizeof(vec3));
	int breakCount= 0;
	
	while((ch = fgetc(fp)) != EOF)
	{
		read = fscanf(fp, "%c",&ch);
		if(read == 1 && ch == 'o' && breakCount == 1)
		{
			breakCount = 1;
			
		}
		if(read == 1 && ch == 'v')
		{
			fscanf(fp, "%f", &x);
			fscanf(fp, "%f", &y);
			fscanf(fp, "%f", &z);
			verts[i].x = x;
			verts[i].y = y;
			verts[i].z = z;
			//printf("%f, %f, %f\n", verts[i].x, verts[i].y, verts[i].z);
			i++;
		}
		if(read == 1 && ch == 'f')
		{
			fscanf(fp, "%d", &ix);
			fscanf(fp, "%d", &iy);
			fscanf(fp, "%d", &iz);
			faces[j].x = ix;
			faces[j].y = iy;
			faces[j].z = iz;
			//printf("%f, %f, %f\n", faces[j].x, faces[j].y, faces[j].z);
			j++;
		}
	}
	
	fclose(fp);
	
	//vec3 normals[vertCount*3];
	//vec3 points[vertCount*3];
	vec3 one, two;
	int Index = 0;
	int a, b, c;
	
	newObj.normals = malloc(faceCount*3*sizeof(vec3));
	newObj.points = malloc(faceCount*3*sizeof(vec3));
	
	for(int i = 0; i < faceCount; i++)
	{
		a = faces[i].x-1;
		b = faces[i].y-1;
		c = faces[i].z-1;
		
		one.x = verts[b].x - verts[a].x;
		one.y = verts[b].y - verts[a].y;
		one.z = verts[b].z - verts[a].z;
	
		two.x = verts[c].x - verts[b].x;
		two.y = verts[c].y - verts[b].y;
		two.z = verts[c].z - verts[b].z;

		vec3 normal = normalizevec3(crossvec3(one, two));

		newObj.normals[Index] = normal; newObj.points[Index] = verts[a]; Index++;
		newObj.normals[Index] = normal; newObj.points[Index] = verts[b]; Index++;
		newObj.normals[Index] = normal; newObj.points[Index] = verts[c]; Index++;
	}
	
	free(verts);
	free(faces);
	
	newObj.size = Index*sizeof(vec3);
	newObj.nsize = Index*sizeof(vec3);
	newObj.vertexNumber = Index;
	return newObj;
}

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>

struct Vec3
{
    float x;
    float y;
    float z;
};

struct Face
{
    unsigned int a;
    unsigned int b;
    unsigned int c;
    std::string material;
};

struct Mesh
{
    std::vector<Vec3> vertices;
    std::vector<Face> faces;
};

const double PI = 3.14159265358979323846;

unsigned int AddVertex(Mesh& mesh, float x, float y, float z)
{
    mesh.vertices.push_back({x, y, z});
    return static_cast<unsigned int>(mesh.vertices.size() - 1);
}

void AddFace(
    Mesh& mesh,
    unsigned int a,
    unsigned int b,
    unsigned int c,
    const std::string& material
)
{
    mesh.faces.push_back({a, b, c, material});
}

std::vector<unsigned int> AddRing(
    Mesh& mesh,
    float z,
    float radius,
    int segments
)
{
    std::vector<unsigned int> ring;

    for (int i = 0; i < segments; i++)
    {
        double angle = (2.0 * PI * i) / segments;

        float x = static_cast<float>(std::cos(angle) * radius);
        float y = static_cast<float>(std::sin(angle) * radius);

        ring.push_back(AddVertex(mesh, x, y, z));
    }

    return ring;
}

void AddSideSurface(
    Mesh& mesh,
    const std::vector<unsigned int>& backRing,
    const std::vector<unsigned int>& frontRing,
    const std::string& material
)
{
    int segments = static_cast<int>(backRing.size());

    for (int i = 0; i < segments; i++)
    {
        int next = (i + 1) % segments;

        unsigned int backA = backRing[i];
        unsigned int backB = backRing[next];
        unsigned int frontA = frontRing[i];
        unsigned int frontB = frontRing[next];

        AddFace(mesh, backA, backB, frontB, material);
        AddFace(mesh, backA, frontB, frontA, material);
    }
}

void AddTipSurface(
    Mesh& mesh,
    const std::vector<unsigned int>& baseRing,
    unsigned int tipVertex,
    const std::string& material
)
{
    int segments = static_cast<int>(baseRing.size());

    for (int i = 0; i < segments; i++)
    {
        int next = (i + 1) % segments;

        AddFace(mesh, baseRing[i], baseRing[next], tipVertex, material);
    }
}

void AddBackDisk(
    Mesh& mesh,
    unsigned int center,
    const std::vector<unsigned int>& ring,
    const std::string& material
)
{
    int segments = static_cast<int>(ring.size());

    for (int i = 0; i < segments; i++)
    {
        int next = (i + 1) % segments;

        AddFace(mesh, center, ring[next], ring[i], material);
    }
}

void AddBackAnnulus(
    Mesh& mesh,
    const std::vector<unsigned int>& outerRing,
    const std::vector<unsigned int>& innerRing,
    const std::string& material
)
{
    int segments = static_cast<int>(outerRing.size());

    for (int i = 0; i < segments; i++)
    {
        int next = (i + 1) % segments;

        AddFace(mesh, outerRing[i], innerRing[i], innerRing[next], material);
        AddFace(mesh, outerRing[i], innerRing[next], outerRing[next], material);
    }
}

void AddFrontAnnulus(
    Mesh& mesh,
    const std::vector<unsigned int>& outerRing,
    const std::vector<unsigned int>& innerRing,
    const std::string& material
)
{
    int segments = static_cast<int>(outerRing.size());

    for (int i = 0; i < segments; i++)
    {
        int next = (i + 1) % segments;

        AddFace(mesh, outerRing[i], innerRing[next], innerRing[i], material);
        AddFace(mesh, outerRing[i], outerRing[next], innerRing[next], material);
    }
}

void SaveMTL(const std::string& filename)
{
    std::ofstream file(filename);

    if (!file.is_open())
    {
        std::cout << "Failed to create MTL file." << std::endl;
        return;
    }

    file << "newmtl Brass\n";
    file << "Ka 0.85 0.60 0.20\n";
    file << "Kd 0.95 0.68 0.28\n";
    file << "Ks 0.90 0.80 0.45\n";
    file << "Ns 96\n\n";

    file << "newmtl DarkBrass\n";
    file << "Ka 0.45 0.30 0.10\n";
    file << "Kd 0.55 0.36 0.13\n";
    file << "Ks 0.45 0.35 0.20\n";
    file << "Ns 48\n\n";

    file << "newmtl Copper\n";
    file << "Ka 0.70 0.30 0.16\n";
    file << "Kd 0.86 0.38 0.20\n";
    file << "Ks 0.75 0.45 0.30\n";
    file << "Ns 90\n\n";

    file << "newmtl DarkCopper\n";
    file << "Ka 0.35 0.15 0.08\n";
    file << "Kd 0.48 0.20 0.10\n";
    file << "Ks 0.35 0.20 0.12\n";
    file << "Ns 45\n\n";

    file << "newmtl Steel\n";
    file << "Ka 0.45 0.45 0.45\n";
    file << "Kd 0.65 0.65 0.65\n";
    file << "Ks 0.85 0.85 0.85\n";
    file << "Ns 110\n\n";

    file << "newmtl Primer\n";
    file << "Ka 0.05 0.05 0.05\n";
    file << "Kd 0.10 0.10 0.10\n";
    file << "Ks 0.25 0.25 0.25\n";
    file << "Ns 25\n";

    file.close();

    std::cout << "Saved material file as " << filename << std::endl;
}

void SaveOBJ(
    const Mesh& mesh,
    const std::string& objFilename,
    const std::string& mtlFilename
)
{
    std::ofstream file(objFilename);

    if (!file.is_open())
    {
        std::cout << "Failed to create OBJ file." << std::endl;
        return;
    }

    file << "# Realistic bullet cartridge generated from C++\n";
    file << "mtllib " << mtlFilename << "\n";
    file << "o Realistic_Bullet_Cartridge\n\n";

    for (const Vec3& vertex : mesh.vertices)
    {
        file << "v "
             << vertex.x << " "
             << vertex.y << " "
             << vertex.z << "\n";
    }

    file << "\n";
    file << "s 1\n";

    std::string currentMaterial;

    for (const Face& face : mesh.faces)
    {
        if (face.material != currentMaterial)
        {
            currentMaterial = face.material;
            file << "\nusemtl " << currentMaterial << "\n";
        }

        file << "f "
             << face.a + 1 << " "
             << face.b + 1 << " "
             << face.c + 1 << "\n";
    }

    file.close();

    std::cout << "Saved object file as " << objFilename << std::endl;
}

Mesh CreateRealisticBulletMesh()
{
    Mesh mesh;

    const int segments = 96;

    std::vector<unsigned int> rimBack = AddRing(mesh, 0.000f, 0.140f, segments);
    std::vector<unsigned int> rimFront = AddRing(mesh, 0.045f, 0.140f, segments);

    AddSideSurface(mesh, rimBack, rimFront, "Brass");

    std::vector<unsigned int> grooveBack = AddRing(mesh, 0.060f, 0.118f, segments);
    std::vector<unsigned int> grooveFront = AddRing(mesh, 0.085f, 0.118f, segments);

    AddSideSurface(mesh, rimFront, grooveBack, "Brass");
    AddSideSurface(mesh, grooveBack, grooveFront, "DarkBrass");

    std::vector<unsigned int> caseBase = AddRing(mesh, 0.110f, 0.128f, segments);
    std::vector<unsigned int> caseBody = AddRing(mesh, 0.610f, 0.128f, segments);

    AddSideSurface(mesh, grooveFront, caseBase, "Brass");
    AddSideSurface(mesh, caseBase, caseBody, "Brass");

    std::vector<unsigned int> shoulder = AddRing(mesh, 0.700f, 0.108f, segments);
    std::vector<unsigned int> neckBack = AddRing(mesh, 0.745f, 0.096f, segments);
    std::vector<unsigned int> neckFront = AddRing(mesh, 0.800f, 0.096f, segments);

    AddSideSurface(mesh, caseBody, shoulder, "Brass");
    AddSideSurface(mesh, shoulder, neckBack, "Brass");
    AddSideSurface(mesh, neckBack, neckFront, "Brass");

    std::vector<unsigned int> projectileBase = AddRing(mesh, 0.800f, 0.088f, segments);

    AddFrontAnnulus(mesh, neckFront, projectileBase, "Brass");

    std::vector<unsigned int> projectileStraight = AddRing(mesh, 0.865f, 0.088f, segments);
    std::vector<unsigned int> cannelureIn = AddRing(mesh, 0.885f, 0.080f, segments);
    std::vector<unsigned int> cannelureOut = AddRing(mesh, 0.910f, 0.080f, segments);
    std::vector<unsigned int> projectileAfterGroove = AddRing(mesh, 0.930f, 0.087f, segments);

    AddSideSurface(mesh, projectileBase, projectileStraight, "Copper");
    AddSideSurface(mesh, projectileStraight, cannelureIn, "DarkCopper");
    AddSideSurface(mesh, cannelureIn, cannelureOut, "DarkCopper");
    AddSideSurface(mesh, cannelureOut, projectileAfterGroove, "Copper");

    float noseStartZ = 0.930f;
    float noseLength = 0.230f;
    float baseRadius = 0.087f;

    std::vector<unsigned int> previousRing = projectileAfterGroove;

    const int noseSteps = 16;

    for (int i = 1; i < noseSteps; i++)
    {
        float t = static_cast<float>(i) / static_cast<float>(noseSteps);

        float z = noseStartZ + noseLength * t;

        float curve = static_cast<float>(
            std::pow(std::cos((PI * 0.5) * t), 0.58)
        );

        float radius = baseRadius * curve;

        std::vector<unsigned int> ring = AddRing(mesh, z, radius, segments);

        AddSideSurface(mesh, previousRing, ring, "Copper");

        previousRing = ring;
    }

    unsigned int tip = AddVertex(mesh, 0.0f, 0.0f, noseStartZ + noseLength);
    AddTipSurface(mesh, previousRing, tip, "Copper");

    unsigned int primerCenter = AddVertex(mesh, 0.0f, 0.0f, 0.0f);

    std::vector<unsigned int> primerInner = AddRing(mesh, 0.000f, 0.045f, segments);
    std::vector<unsigned int> primerOuter = AddRing(mesh, 0.000f, 0.068f, segments);

    AddBackDisk(mesh, primerCenter, primerInner, "Primer");
    AddBackAnnulus(mesh, primerOuter, primerInner, "Steel");
    AddBackAnnulus(mesh, rimBack, primerOuter, "Brass");

    return mesh;
}

int main()
{
    Mesh bulletMesh = CreateRealisticBulletMesh();

    SaveMTL("realistic_bullet.mtl");
    SaveOBJ(bulletMesh, "realistic_bullet.obj", "realistic_bullet.mtl");

    std::cout << "\nBullet mesh created successfully!" << std::endl;
    std::cout << "Total vertices: " << bulletMesh.vertices.size() << std::endl;
    std::cout << "Total triangles: " << bulletMesh.faces.size() << std::endl;

    std::cout << "\nOpen realistic_bullet.obj in Blender or another 3D viewer." << std::endl;
    std::cout << "Keep realistic_bullet.obj and realistic_bullet.mtl in the same folder." << std::endl;

    return 0;
}
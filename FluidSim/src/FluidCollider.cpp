#include "FluidCollider.h"

#include "FluidCode.h"
#include "FluidSystem.h"


using namespace trace;


FluidCollider::FluidCollider()
{
	vel = Vec2(40, 0);
}


FluidCollider::~FluidCollider()
{
}

void FluidCollider::Update(int N, FluidSystem* pFs, std::vector<byte>& cellInfo, real dt)
{
	loc += vel * dt;
	rot += angVel * dt;

	for (int i = 0; i < 2; i++) {
		if (loc(i) > N + scale(i)) {
			loc(i) = -scale(i);
		}
		if (loc(i) < -scale(i)) {
			loc(i) += N+2;
		}
	}

	rotMatrix <<
		cos(rot), -sin(rot), 0,
		sin(rot), cos(rot), 0,
		0, 0, 1;
	scMatrix <<
		scale(0), 0, 0,
		0, scale(1), 0,
		0, 0, 1;
	locMatrix <<
		1, 0, loc(0),
		0, 1, loc(1),
		0, 0, 1;
	Eigen::Matrix3f modelMatrix = locMatrix * scMatrix * rotMatrix;

	auto center = modelMatrix * Vec3(0,0,1);
	ms_center = Vec2(center(0), center(1));

	gTracePointsWS.clear();
	for (auto& ms_normal : modelSpacePoints) {
		auto ws_point = modelMatrix * ms_normal;
		gTracePointsWS.push_back(V2f{ ws_point(0), ws_point(1) });
	}

	normals.clear();
	for (auto& ms_normal : modelSpaceNormals) {
		auto ws_norm = modelMatrix * ms_normal;
		auto tmp = Vec2{ ws_norm(0), ws_norm(1) };
		tmp.normalize();
		normals.push_back(tmp);
	}

	for (V2f& p : gTracePointsWS) {
		if (p.x < cellOffsetX) {
			cellOffsetX = p.x;
		}
		if (p.y < cellOffsetY) {
			cellOffsetY = p.y;
		}
	}

	for (V2f& p : gTracePointsWS) {
		p.x -= cellOffsetX;
		p.y -= cellOffsetY;
	}

	size_t cell_size = 1;
	gCells = pick_cells(gTracePointsWS, cell_size);
	mass = 10 * gCells.size() / (real)(N*N);

	for (auto c : gCells) {
		int x = c.x + cellOffsetX;
		int y = c.y + cellOffsetY;
		if (x <= N && y <= N && x > 0 && y > 0) {

			int id = x + (N + 2) * y;
			cellInfo[id] &= ~CellInfo::FLUID;
			cellInfo[id] |= CellInfo::OBJECT;

			auto effective_vel = vel * 0.05;
			pFs->SetVelocity(x, y, effective_vel(0), effective_vel(1));
		}
	}

	UpdateChild(N, pFs, cellInfo, dt);
}

void FluidCollider::AddVel(int N, real& torque, Vec2& force, int x, int y, std::vector<real>& vX, std::vector<real>& vY, FluidSystem* pFs) {
	real d = 0.1 + Tools::Min(pFs->CombinedDensity(x, y), 0.9f);
	int id = x + (N + 2) * y;
	auto vel = Vec2{ vX[id], vY[id] };
	force += vel;
	auto arm = Vec2(x, y) - ms_center;
	torque += d * ( (arm(0) * vel(1) - arm(1)*vel(0)) / (arm(0)*arm(0) + arm(1)*arm(1))); //(Vec2(x,y) - ms_center) * vel;
}

void FluidCollider::ApplyForces(int N, real dt, FluidSystem* pFs)
{
	// for all cells, find neighbouring fluid cells
	auto& infos = pFs->GetInfo();

	real torque{ 0 };
	Vec2 force { 0,0 };

	auto& vX = pFs->GetVelX();
	auto& vY = pFs->GetVelY();

	for (auto c : gCells) {
		int x = c.x + cellOffsetX;
		int y = c.y + cellOffsetY;
		if (x <= N && y <= N && x > 0 && y > 0) {
			auto info = infos[x + (N + 2) * y];
			
			if (!(info & LEFT)) {
				AddVel(N, torque, force, x - 1, y, vX, vY, pFs);
			}
			if (!(info & RIGHT)) {
				AddVel(N, torque, force, x + 1, y, vX, vY, pFs);
			}
			if (!(info & TOP)) {
				AddVel(N, torque, force, x, y + 1, vX, vY, pFs);
			}
			if (!(info & BOTTOM)) {
				AddVel(N, torque, force, x, y - 1, vX, vY, pFs);
			}
		}
	}

	this->vel += dt * force / mass;
	this->vel *= powf(0.9, dt);
	this->angVel += dt * torque;
	this->angVel *= powf(0.9, dt);
}

bool FluidCollider::Contains(int i, int j)
{
	return gCells.find(V2i{ i,j }) != gCells.end();
}

void FluidCollider::AddVel(real dx, real dy)
{
	vel += Vec2{dx, dy};
}

void FluidCollider::Collide(FluidCollider* A, FluidCollider* B)
{
	uint32_t face_idx_1, face_idx_2;
	real distance1 = A->FindAxisLeastPenetration(&face_idx_1, B);
	real distance2 = B->FindAxisLeastPenetration(&face_idx_2, A);
	real distance = Tools::Max(distance1, distance2);
	Vec2 normal = distance1 > distance2 ? A->normals[face_idx_1] : B->normals[face_idx_2];

	std::cout << " dist1: " << distance1 << " dist2: " << distance2 << "\r";
	if (distance < 0) {
		// collision response
		FluidCollider::ApplyImpulse(A, B, normal);
	}
}

void FluidCollider::ApplyImpulse(FluidCollider* A, FluidCollider* B, Vec2 normal)
{
	Vec2 relVel = A->vel - B->vel;
	real contactVel = relVel.dot(normal);

	if (contactVel > 0 || A->mass < 0.000001 || B->mass < 0.000001)
		return;

	// Calculate restitution
	real e = std::min(A->coeff_restitution, B->coeff_restitution);

	// Calculate impulse scalar
	real j = -(1.0f + e) * contactVel;
	j /= 1 / A->mass + 1 / B->mass + pow(1, 2)/A->momentOfInertia + pow(1, 2)/B->momentOfInertia;

	// Apply impulse
	Vec2 impulse = j * normal;
	A->vel -= 1 / A->mass * impulse;
	B->vel += 1 / B->mass * impulse;
}

real FluidCollider::FindAxisLeastPenetration(uint32_t *faceIndex, FluidCollider* other)
{
	real bestDistance = -FLT_MAX;
	uint32_t bestIndex;

	for (uint32_t i = 0; i < this->gTracePointsWS.size(); ++i)
	{
		// Retrieve a face normal from A
		Vec2 n = this->normals[i];

		// Retrieve support point from B along -n
		Vec2 s = other->GetSupport(-n);

		// Retrieve vertex on face from A, transform into
		// B's model space
		Vec2 v = this->gTracePointsWS[i].toVec2();

		// Compute penetration distance (in B's model space)
		real d = n.dot(s - v);// Dot(n, s - v);

		// Store greatest distance
		if (d > bestDistance)
		{
			bestDistance = d;
			bestIndex = i;
		}
	}

	*faceIndex = bestIndex;
	return bestDistance;
}

Vec2 FluidCollider::GetSupport(const Vec2& dir)
{
	real bestProjection = -FLT_MAX;
	Vec2 bestVertex;

	for (int i = 0; i < gTracePointsWS.size(); ++i)
	{
		auto v = gTracePointsWS[i].toVec2();
		real projection = v.dot(dir);

		if (projection > bestProjection)
		{
			bestVertex = v;
			bestProjection = projection;
		}
	}

	return bestVertex;
}


RectCollider::RectCollider(int ox, int oy, real w, real h, real r)
{
	loc = Vec2(ox, oy);
	scale = Vec2(w, h);
	rot = r;

	modelSpacePoints = { Vec3(-0.5, -0.5, 1), Vec3(-0.5, 0.5, 1), Vec3(0.5, 0.5, 1), Vec3(0.5, -0.5, 1) };
	modelSpaceNormals = { Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(0, -1, 0) };
}

void RectCollider::UpdateChild(int N, FluidSystem* pFs, std::vector<byte>& cellInfo, real dt)
{


}

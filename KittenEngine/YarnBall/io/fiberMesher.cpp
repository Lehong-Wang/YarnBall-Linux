#include "../YarnBall.h"
#include "fastPRNG.h"

namespace YarnBall {
	inline float randf(fastPRNG::fastXS64& rng) {
		return glm::clamp((uint32_t)rng.xoroshiro128p() / (float)UINT32_MAX, 0.f, 1.f);
	}

	void Sim::exportFiberMesh(std::string path) {
		using namespace fastPRNG;
		using Kit::Rotor;

		const int64_t seed = 0x12345678;
		fastXS64 prng(seed);

		const float r = 1.2f * (meta.radius + 0.5f * meta.barrierThickness);

		const int numFibers = 8;
		const float fiberFrizz = 0.2f * r;
		const float fiberTwist = 1200.f;

		const float fuzzDensity = 1.25e4;
		const float fuzzLength = 1.f * r;

		const float fiberRadius = 0.6f * r / sqrt((float)numFibers);
		const float heightSegLen = 0.3f * r;

		FILE* file = fopen(path.c_str(), "w");
		if (!file) throw std::runtime_error("Failed to open file for writing");

		fprintf(file, "o fiber\n");

		download();

		float fiberOrbit = 0.9f * r - fiberRadius - fiberFrizz;
		size_t numVerts = 0;
		for (int fid = 0; fid < numFibers; fid++) {
			float curTwist = 0;
			size_t vertStart = numVerts;
			for (size_t i = 0; i < meta.numVerts; i++) {
				auto v0 = verts[i];
				if (!(v0.flags & (uint32_t)VertexFlags::hasNext)) {
					if (numVerts - vertStart > 2) {
						fprintf(file, "l");
						for (size_t i = vertStart; i < numVerts; i++)
							fprintf(file, " %zd", i + 1);
						fprintf(file, "\n");
					}
					vertStart = numVerts;
					continue;
				}

				auto v1 = verts[i + 1];

				Rotor q[3]{};
				q[0] = q[1] = q[2] = qs[i];

				auto va = v0;
				auto vb = v1;
				if (v0.flags & (uint32_t)VertexFlags::hasPrev) {
					va = verts[i - 1];
					q[0] = qs[i - 1];
				}
				if (v1.flags & (uint32_t)VertexFlags::hasNext) {
					vb = verts[i + 2];
					q[2] = qs[i + 1];
				}

				int numHeightSegs = glm::max((int)glm::ceil(v0.lRest / heightSegLen), 1);

				// Add fibers for this segment
				Rotor lastFrame;
				for (int hi = 0; hi < numHeightSegs; hi++) {
					// CMR3 interpolation to get position
					float t = hi / (float)numHeightSegs;
					vec3 pos = Kit::cmrSpline(va.pos, v0.pos, v1.pos, vb.pos, t);
					Rotor frame = q[1];
					if (t < 0.5f) frame = normalize(mix(q[0].v, frame.v, t + 0.5f));
					else frame = normalize(mix(frame.v, q[2].v, t - 0.5f));

					float curTwistAngle = curTwist + fiberTwist * t * v0.lRest;
					curTwistAngle += fid / (float)numFibers * 2 * glm::pi<float>();
					vec3 twist(0, cos(curTwistAngle), sin(curTwistAngle));
					twist *= fiberOrbit;

					if (hi && hi != numHeightSegs)
						twist += fiberFrizz * (vec3(randf(prng), randf(prng), randf(prng)) * 2.f - 1.f);

					// Write out a circle in obj format
					vec3 wPos = pos + frame * twist;
					fprintf(file, "v %.8f %.8f %.8f\n", wPos.x, wPos.y, wPos.z);
					numVerts++;

					lastFrame = frame;
				}

				curTwist += fiberTwist * v0.lRest;
				curTwist = glm::mod(curTwist, 2 * glm::pi<float>());
			}
		}

		// Second pass to add fuzz
		fprintf(file, "o fuzz\n");
		float curTwist = 0;
		for (size_t i = 0; i < meta.numVerts; i++) {
			auto v0 = verts[i];
			if (!(v0.flags & (uint32_t)VertexFlags::hasNext)) continue;

			auto v1 = verts[i + 1];

			Rotor q[3]{};
			q[0] = q[1] = q[2] = qs[i];

			auto va = v0;
			auto vb = v1;
			if (v0.flags & (uint32_t)VertexFlags::hasPrev) {
				va = verts[i - 1];
				q[0] = qs[i - 1];
			}
			if (v1.flags & (uint32_t)VertexFlags::hasNext) {
				vb = verts[i + 2];
				q[2] = qs[i + 1];
			}

			int numFuzz = (int)round(2 * randf(prng) * v0.lRest * fuzzDensity);
			for (int i = 0; i < numFuzz; i++) {
				// Randomly generate fuzz orientation and position
				float t = randf(prng);
				vec3 pos = Kit::cmrSpline(va.pos, v0.pos, v1.pos, vb.pos, t);
				Rotor frame = q[1];
				if (t < 0.5f) frame = normalize(mix(q[0].v, frame.v, t + 0.5f));
				else frame = normalize(mix(frame.v, q[2].v, t - 0.5f));

				vec3 fuzzDir = vec3(randf(prng), randf(prng), randf(prng)) * 2.f - 1.f;
				vec3 offset = fuzzDir + vec3(randf(prng), randf(prng), randf(prng)) * 2.f - 1.f;
				offset.x *= 0.1f;
				offset = 0.9f * fiberOrbit * normalize(offset);
				fuzzDir *= fuzzLength * (0.8f + 0.2f * randf(prng));

				for (int hi = 0; hi <= 1; hi++) {
					float t = hi;

					// Write out a circle in obj format
					vec3 wPos = pos + frame * (t * fuzzDir + offset);
					fprintf(file, "v %.8f %.8f %.8f\n", wPos.x, wPos.y, wPos.z);
					numVerts++;
				}

				fprintf(file, "l %zd %zd\n", numVerts - 1, numVerts);
			}

			curTwist += fiberTwist * v0.lRest;
			curTwist = glm::mod(curTwist, 2 * glm::pi<float>());
		}

		fclose(file);
	}
}
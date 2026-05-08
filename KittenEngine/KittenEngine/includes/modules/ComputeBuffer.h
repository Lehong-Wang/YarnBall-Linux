#pragma once
// Jerry Hsu, 2021

#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>

#if __has_include("cuda_runtime.h")
#include <cuda_runtime.h>
#include <cuda_gl_interop.h>
#include <cstring>
#include <vector>
#endif

namespace Kitten {
#ifdef __CUDA_RUNTIME_H__
	// True on GL stacks that don't expose NVIDIA's CUDA-GL interop bridge,
	// where cudaGraphicsGLRegisterBuffer destroys the CUDA context. Detected
	// by GL_RENDERER substring; native NVIDIA on Windows/Linux returns false
	// and the existing fast interop path runs unchanged.
	inline bool cudaGLInteropBroken() {
		static int decided = -1;
		if (decided < 0) {
			const char* renderer = (const char*)glGetString(GL_RENDERER);
			decided = (renderer &&
				(std::strstr(renderer, "D3D12") ||      // WSLg's Mesa-on-D3D12
				 std::strstr(renderer, "llvmpipe")))    // software fallback
				? 1 : 0;
		}
		return decided == 1;
	}
#endif

	class ComputeBuffer {
	public:
		unsigned int glHandle;
		size_t elementSize, size;
		GLenum usage;

		ComputeBuffer() = delete;
		ComputeBuffer(size_t elementSize, size_t count, GLenum usage = GL_DYNAMIC_READ);
		~ComputeBuffer();

		void bind(int loc);
		void resize(size_t newSize);
		void upload(void* src);
		void upload(void* src, size_t count);
		void download(void* dst);
		void download(void* dst, size_t count);

#ifdef __CUDA_RUNTIME_H__
		// These are provided as an alternative to CudaComputerBuffer for compatibility reasons
		void cudaWriteGL(void* ptr, size_t dataSize) {
			if (cudaGLInteropBroken()) {
				thread_local std::vector<uint8_t> host;
				if (host.size() < dataSize) host.resize(dataSize);
				cudaMemcpy(host.data(), ptr, dataSize, cudaMemcpyDeviceToHost);
				glBindBuffer(GL_SHADER_STORAGE_BUFFER, glHandle);
				glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, dataSize, host.data());
				glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
				return;
			}
			cudaGraphicsResource* cudaRes;
			void* cudaPtr;
			cudaGraphicsGLRegisterBuffer(&cudaRes, glHandle, cudaGraphicsRegisterFlagsNone);
			cudaGraphicsMapResources(1, &cudaRes);

			size_t tmp;
			cudaGraphicsResourceGetMappedPointer(&cudaPtr, &tmp, cudaRes);
			cudaMemcpy(cudaPtr, ptr, dataSize, cudaMemcpyDeviceToDevice);

			cudaGraphicsUnmapResources(1, &cudaRes, 0);
			cudaGraphicsUnregisterResource(cudaRes);
		}

		void cudaWriteGL(void* ptr) { cudaWriteGL(ptr, elementSize * size); }

		void cudaReadGL(void* ptr, size_t dataSize) {
			if (cudaGLInteropBroken()) {
				thread_local std::vector<uint8_t> host;
				if (host.size() < dataSize) host.resize(dataSize);
				glBindBuffer(GL_SHADER_STORAGE_BUFFER, glHandle);
				glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, dataSize, host.data());
				glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
				cudaMemcpy(ptr, host.data(), dataSize, cudaMemcpyHostToDevice);
				return;
			}
			cudaGraphicsResource* cudaRes;
			void* cudaPtr;
			cudaGraphicsGLRegisterBuffer(&cudaRes, glHandle, cudaGraphicsRegisterFlagsNone);
			cudaGraphicsMapResources(1, &cudaRes);

			size_t tmp;
			cudaGraphicsResourceGetMappedPointer(&cudaPtr, &tmp, cudaRes);
			cudaMemcpy(ptr, cudaPtr, dataSize, cudaMemcpyDeviceToDevice);

			cudaGraphicsUnmapResources(1, &cudaRes, 0);
			cudaGraphicsUnregisterResource(cudaRes);
		}

		void cudaReadGL(void* ptr) { cudaReadGL(ptr, elementSize * size); }
#endif
	};

#ifdef __CUDA_RUNTIME_H__
	class CudaComputeBuffer : public ComputeBuffer {
	public:
		cudaGraphicsResource* cudaRes;
		void* cudaPtr;

		CudaComputeBuffer() = delete;
		CudaComputeBuffer(size_t elementSize, size_t count, GLenum usage = GL_DYNAMIC_READ) :
			ComputeBuffer(elementSize, count, usage) {
			cudaGraphicsGLRegisterBuffer(&cudaRes, glHandle, cudaGraphicsRegisterFlagsNone);
			cudaGraphicsMapResources(1, &cudaRes);

			size_t size;
			cudaGraphicsResourceGetMappedPointer(&cudaPtr, &size, cudaRes);
		}

		~CudaComputeBuffer() {
			cudaGraphicsUnmapResources(1, &cudaRes, 0);
			cudaGraphicsUnregisterResource(cudaRes);
		}
	};
#endif

}
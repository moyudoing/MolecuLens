// =============================================================================
// MolecuLens · engine · style/quicksurf/quicksurf_cuda.cu —— QuickSurf CUDA 密度核
// 规格来源：QS.2 参照 CUDAQuickSurf.cu（高斯密度 + 梯度 + 密度加权原子色）
// 仅当 CMake 选项 MOLECULENS_QUICKSURF_CUDA=ON 且找到 CUDA 工具链时由 nvcc 编译；
// 否则不编译（quicksurf.cpp 中对应引用由 #if defined(MOLECULENS_QUICKSURF_CUDA) 屏蔽）。
// 运行时经 cudaGetDeviceCount 检测 GPU 可用性 → dispatch 自动选择本后端。
// =============================================================================

#include "engine/style/quicksurf/quicksurf.h"

#include <cuda_runtime.h>

namespace moleculens::engine {

namespace {

// 密度核：每线程处理 (atom, y, z)，内层沿 x 累加；体素可能被多原子写 → atomicAdd
__global__ void quicksurf_density_kernel(
    const float* __restrict__ xyzr, const float* __restrict__ colors, int natoms,
    float radscale, float gridspacing, float gausslim,
    float ox, float oy, float oz, int nx, int ny, int nz,
    float* density, float* grad, float* colormap) {
    const int atom = blockIdx.x * blockDim.x + threadIdx.x;
    const int yz = blockIdx.y * blockDim.y + threadIdx.y;
    if (atom >= natoms) {
        return;
    }
    const int y = yz / nz;
    const int z = yz % nz;
    if (y >= ny || z >= nz) {
        return;
    }
    const float cx = xyzr[atom * 4], cy = xyzr[atom * 4 + 1], cz = xyzr[atom * 4 + 2];
    const float r = xyzr[atom * 4 + 3] * radscale;
    if (r < 1e-4f) {
        return;
    }
    const float acr = colors ? colors[atom * 3] : 1.0f;
    const float acg = colors ? colors[atom * 3 + 1] : 1.0f;
    const float acb = colors ? colors[atom * 3 + 2] : 1.0f;
    const float arinv = 1.0f / (2.0f * r * r);
    const float radlim = gausslim * r;
    const float radlim2 = radlim * radlim;
    const float dz = oz + static_cast<float>(z) * gridspacing - cz;
    const float dy = oy + static_cast<float>(y) * gridspacing - cy;
    const float dy2dz2 = dy * dy + dz * dz;
    if (dy2dz2 >= radlim2) {
        return;
    }
    const float invgs = 1.0f / gridspacing;
    const int x0 = max(0, static_cast<int>(floorf((cx - radlim - ox) * invgs)));
    const int x1 = min(nx - 1, static_cast<int>(ceilf((cx + radlim - ox) * invgs)));
    const size_t base = static_cast<size_t>(z) * nx * ny + static_cast<size_t>(y) * nx;
    for (int x = x0; x <= x1; ++x) {
        const float pcx = ox + static_cast<float>(x) * gridspacing;
        const float r2 = (pcx - cx) * (pcx - cx) + dy2dz2;
        const float g = __expf(-r2 * arinv);
        const size_t addr = base + static_cast<size_t>(x);
        atomicAdd(&density[addr], g);
        atomicAdd(&grad[3 * addr + 0], 2.0f * arinv * (cx - pcx) * g);
        atomicAdd(&grad[3 * addr + 1], 2.0f * arinv * (cy - oy) * g);
        atomicAdd(&grad[3 * addr + 2], 2.0f * arinv * (cz - oz) * g);
        if (colormap) {
            atomicAdd(&colormap[3 * addr + 0], g * acr);
            atomicAdd(&colormap[3 * addr + 1], g * acg);
            atomicAdd(&colormap[3 * addr + 2], g * acb);
        }
    }
}

} // namespace

bool cudaKernelAvailable() {
    int count = 0;
    const cudaError_t e = cudaGetDeviceCount(&count);
    return e == cudaSuccess && count > 0;
}

void computeDensityCuda(const float* xyzr, int natoms, const float* colors,
                        float radscale, float gridspacing, float gausslim,
                        const float origin[3], const int dims[3],
                        float* density, float* grad, float* colormap) {
    const int nx = dims[0], ny = dims[1], nz = dims[2];
    const size_t nvox = static_cast<size_t>(nx) * ny * nz;

    float *d_dens = nullptr, *d_grad = nullptr, *d_cmap = nullptr;
    float *d_xyzr = nullptr, *d_colors = nullptr;
    cudaMalloc(&d_dens, nvox * sizeof(float));
    cudaMalloc(&d_grad, nvox * 3 * sizeof(float));
    cudaMalloc(&d_cmap, nvox * 3 * sizeof(float));
    cudaMemset(d_dens, 0, nvox * sizeof(float));
    cudaMemset(d_grad, 0, nvox * 3 * sizeof(float));
    cudaMemset(d_cmap, 0, nvox * 3 * sizeof(float));
    cudaMalloc(&d_xyzr, static_cast<size_t>(natoms) * 4 * sizeof(float));
    cudaMemcpy(d_xyzr, xyzr, static_cast<size_t>(natoms) * 4 * sizeof(float),
               cudaMemcpyHostToDevice);
    if (colors) {
        cudaMalloc(&d_colors, static_cast<size_t>(natoms) * 3 * sizeof(float));
        cudaMemcpy(d_colors, colors, static_cast<size_t>(natoms) * 3 * sizeof(float),
                   cudaMemcpyHostToDevice);
    }

    dim3 block(64, 8);
    dim3 grid((static_cast<unsigned>(natoms) + 63u) / 64u,
              (static_cast<unsigned>(ny) * static_cast<unsigned>(nz) + 7u) / 8u);
    quicksurf_density_kernel<<<grid, block>>>(
        d_xyzr, d_colors, natoms, radscale, gridspacing, gausslim, origin[0],
        origin[1], origin[2], nx, ny, nz, d_dens, d_grad, d_cmap);
    cudaDeviceSynchronize();

    cudaMemcpy(density, d_dens, nvox * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(grad, d_grad, nvox * 3 * sizeof(float), cudaMemcpyDeviceToHost);
    if (colormap) {
        cudaMemcpy(colormap, d_cmap, nvox * 3 * sizeof(float), cudaMemcpyDeviceToHost);
    }

    cudaFree(d_dens);
    cudaFree(d_grad);
    cudaFree(d_cmap);
    cudaFree(d_xyzr);
    if (d_colors) {
        cudaFree(d_colors);
    }
}

} // namespace moleculens::engine

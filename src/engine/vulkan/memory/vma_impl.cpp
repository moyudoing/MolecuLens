// =============================================================================
// MolecuLens · engine · vulkan/memory/vma_impl.cpp —— VMA 实现单元
// 说明：vcpkg 的 vulkan-memory-allocator 为 header-only（INTERFACE 目标），
//       需在一个 TU 中定义 VMA_IMPLEMENTATION 生成 vma* 符号。
// =============================================================================

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

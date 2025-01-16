/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/dawn/GrDawnRingBuffer.h"

#include "src/gpu/dawn/GrDawnGpu.h"
#include "src/gpu/dawn/GrDawnStagingBuffer.h"
#include "src/gpu/dawn/GrDawnUtil.h"

namespace {
    const int kDefaultSize = 32 * 1024;
}

GrDawnRingBuffer::GrDawnRingBuffer(GrDawnGpu* gpu, wgpu::BufferUsage usage)
    : fGpu(gpu) , fUsage(usage) {
}

GrDawnRingBuffer::~GrDawnRingBuffer() {
}

GrDawnRingBuffer::Slice GrDawnRingBuffer::allocate(int size) {
    if (!fBuffer || fOffset + size > kDefaultSize) {
        wgpu::BufferDescriptor desc;
        desc.usage = fUsage | wgpu::BufferUsage::CopyDst;
        desc.size = kDefaultSize;
        fBuffer = fGpu->device().CreateBuffer(&desc);
        fOffset = 0;
    }

    GrStagingBuffer::Slice staging = fGpu->allocateStagingBufferSlice(size);
    size_t offset = fOffset;
    fOffset += size;
    fOffset = GrDawnRoundRowBytes(fOffset);
    wgpu::Buffer srcBuffer = static_cast<GrDawnStagingBuffer*>(staging.fBuffer)->buffer();
    fGpu->getCopyEncoder().CopyBufferToBuffer(srcBuffer, staging.fOffset,
                                              fBuffer, offset, size);
    return Slice(fBuffer, offset, staging.fData);
}

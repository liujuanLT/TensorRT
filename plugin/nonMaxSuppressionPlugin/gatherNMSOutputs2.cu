/*
 * Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "kernel.h"
#include "plugin.h"
#include "cuda_fp16.h"
#include "gatherNMSOutputs2.h"
#include <array>

// __half minus with fallback to float for old sm
inline __device__ __half minus_fb(const __half & a, const __half & b) {
#if __CUDA_ARCH__ >= 530
    return a - b;
#else
    return __float2half(__half2float(a) - __half2float(b));
#endif
}

template <typename T_BBOX>
__device__ T_BBOX saturate2(T_BBOX v)
{
    return max(min(v, T_BBOX(1)), T_BBOX(0));
}

template <>
__device__ __half saturate2(__half v)
{
#if __CUDA_ARCH__ >= 800
    return __hmax(__hmin(v, __half(1)), __half(0));
#elif __CUDA_ARCH__ >= 530
    return __hge(v, __half(1)) ? __half(1) : (__hle(v, __half(0)) ? __half(0) : v);
#else
    return max(min(v, float(1)), float(0));
#endif
}

template <typename T_BBOX, typename T_SCORE, unsigned nthds_per_cta>
__launch_bounds__(nthds_per_cta)
    __global__ void gatherNMSOutputs2_kernel(
        const bool shareLocation,
        const int numImages,
        const int numPredsPerClass,
        const int numClasses,
        const int topK,
        const int keepTopK,
        const int* indices,
        const T_SCORE* scores,
        const T_BBOX* bboxData,
        int* nmsedResult,
        bool clipBoxes,
        const T_SCORE scoreShift
        )
{
    if (keepTopK > topK)
        return;
    for (int i = blockIdx.x * nthds_per_cta + threadIdx.x;
         i < numImages * keepTopK;
         i += gridDim.x * nthds_per_cta)
    {
        const int imgId = i / keepTopK;
        const int detId = i % keepTopK;
        const int offset = imgId * numClasses * topK;
        const int index = indices[offset + detId]; 
        //const T_SCORE score = scores[offset + detId];
        if (index == -1)
        {
            nmsedResult[i] = -1;
            nmsedResult[i+1] = -1;
            nmsedResult[i+2] = -1;
        }
        else
        {
            const int bboxOffset = imgId * (shareLocation ? numPredsPerClass : (numClasses * numPredsPerClass));
            const int bboxId = ((shareLocation ? (index % numPredsPerClass)
                        : index % (numClasses * numPredsPerClass)) + bboxOffset) * 4;
            const int classId = (index % (numClasses * numPredsPerClass)) / numPredsPerClass; // label
            nmsedResult[i] = imgId;
            nmsedResult[i+1] = classId;
            nmsedResult[i+2] = bboxId;
           
        }
    }
}

template <typename T_BBOX, typename T_SCORE>
pluginStatus_t gatherNMSOutputs2_gpu(
    cudaStream_t stream,
    const bool shareLocation,
    const int numImages,
    const int numPredsPerClass,
    const int numClasses,
    const int topK,
    const int keepTopK,
    const void* indices,
    const void* scores,
    const void* bboxData,
    void* nmsedResult,
    bool clipBoxes,
    const float scoreShift
    )
{
    //cudaMemsetAsync(numDetections, 0, numImages * sizeof(int), stream);
    const int BS = 32;
    const int GS = 32;
    gatherNMSOutputs2_kernel<T_BBOX, T_SCORE, BS><<<GS, BS, 0, stream>>>(shareLocation, numImages, numPredsPerClass,
                                                                           numClasses, topK, keepTopK,
                                                                           (int*) indices, (T_SCORE*) scores, (T_BBOX*) bboxData,
                                                                           (int*) nmsedResult,
                                                                           clipBoxes,
                                                                           T_SCORE(scoreShift)
                                                                            );

    CSC(cudaGetLastError(), STATUS_FAILURE);
    return STATUS_SUCCESS;
}

// gatherNMSOutputs2 LAUNCH CONFIG {{{
typedef pluginStatus_t (*nmsOutFunc)(cudaStream_t,
                               const bool,
                               const int,
                               const int,
                               const int,
                               const int,
                               const int,
                               const void*,
                               const void*,
                               const void*,
                               void*,
                               bool,
                               const float);
struct nmsOutLaunchConfig
{
    DataType t_bbox;
    DataType t_score;
    nmsOutFunc function;

    nmsOutLaunchConfig(DataType t_bbox, DataType t_score)
        : t_bbox(t_bbox)
        , t_score(t_score)
    {
    }
    nmsOutLaunchConfig(DataType t_bbox, DataType t_score, nmsOutFunc function)
        : t_bbox(t_bbox)
        , t_score(t_score)
        , function(function)
    {
    }
    bool operator==(const nmsOutLaunchConfig& other)
    {
        return t_bbox == other.t_bbox && t_score == other.t_score;
    }
};

using nvinfer1::DataType;

static std::array<nmsOutLaunchConfig, 2> nmsOutLCOptions = {
  nmsOutLaunchConfig(DataType::kFLOAT, DataType::kFLOAT, gatherNMSOutputs2_gpu<float, float>),
  nmsOutLaunchConfig(DataType::kHALF, DataType::kHALF, gatherNMSOutputs2_gpu<__half, __half>)
};

pluginStatus_t gatherNMSOutputs2(
    cudaStream_t stream,
    const bool shareLocation,
    const int numImages,
    const int numPredsPerClass,
    const int numClasses,
    const int topK,
    const int keepTopK,
    const DataType DT_BBOX,
    const DataType DT_SCORE,
    const void* indices,
    const void* scores,
    const void* bboxData,
    void* nmsedResult,
    bool clipBoxes,
    const float scoreShift
    )
{
    nmsOutLaunchConfig lc = nmsOutLaunchConfig(DT_BBOX, DT_SCORE);
    for (unsigned i = 0; i < nmsOutLCOptions.size(); ++i)
    {
        if (lc == nmsOutLCOptions[i])
        {
            DEBUG_PRINTF("gatherNMSOutputs2 kernel %d\n", i);
            return nmsOutLCOptions[i].function(stream,
                                          shareLocation,
                                          numImages,
                                          numPredsPerClass,
                                          numClasses,
                                          topK,
                                          keepTopK,
                                          indices,
                                          scores,
                                          bboxData,
                                          nmsedResult,
                                          clipBoxes,
                                          scoreShift
                                          );
        }
    }
    return STATUS_BAD_PARAM;
}

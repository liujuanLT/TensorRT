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

#include "nonMaxSuppressionPlugin.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

using namespace nvinfer1;
using nvinfer1::plugin::NonMaxSuppressionBasePluginCreator;
using nvinfer1::plugin::NonMaxSuppressionDynamicPlugin;
using nvinfer1::plugin::NonMaxSuppressionDynamicPluginCreator;
using nvinfer1::plugin::NonMaxSuppressionPlugin;
using nvinfer1::plugin::NonMaxSuppressionPluginCreator;
using nvinfer1::plugin::NMSParameters;

namespace
{
const char* NMS_PLUGIN_VERSION{"1"};
const char* NMS_PLUGIN_NAMES[] = {"NonMaxSuppression_TRT", "NonMaxSuppressionDynamic_TRT"};
} // namespace

PluginFieldCollection NonMaxSuppressionBasePluginCreator::mFC{};
std::vector<PluginField> NonMaxSuppressionBasePluginCreator::mPluginAttributes;

NonMaxSuppressionPlugin::NonMaxSuppressionPlugin(NMSParameters params) noexcept
    : param(params)
{
}

NonMaxSuppressionPlugin::NonMaxSuppressionPlugin(const void* data, size_t length) noexcept
{
    const char *d = reinterpret_cast<const char*>(data), *a = d;
    param = read<NMSParameters>(d);
    boxesSize = read<int>(d);
    scoresSize = read<int>(d);
    numPriors = read<int>(d);
    mClipBoxes = read<bool>(d);
    mPrecision = read<DataType>(d);
    mScoreBits = read<int32_t>(d);
    ASSERT(d == a + length);
}

NonMaxSuppressionDynamicPlugin::NonMaxSuppressionDynamicPlugin(NMSParameters params) noexcept
    : param(params)
{
}

NonMaxSuppressionDynamicPlugin::NonMaxSuppressionDynamicPlugin(const void* data, size_t length) noexcept
{
    const char *d = reinterpret_cast<const char*>(data), *a = d;
    param = read<NMSParameters>(d);
    boxesSize = read<int>(d);
    scoresSize = read<int>(d);
    numPriors = read<int>(d);
    mClipBoxes = read<bool>(d);
    mPrecision = read<DataType>(d);
    mScoreBits = read<int32_t>(d);
    ASSERT(d == a + length);
}

int NonMaxSuppressionPlugin::getNbOutputs() const noexcept
{
    return 4;
}

int NonMaxSuppressionDynamicPlugin::getNbOutputs() const noexcept
{
    return 1;
}

int NonMaxSuppressionPlugin::initialize() noexcept
{
    return STATUS_SUCCESS;
}

int NonMaxSuppressionDynamicPlugin::initialize() noexcept
{
    return STATUS_SUCCESS;
}

void NonMaxSuppressionPlugin::terminate() noexcept {}

void NonMaxSuppressionDynamicPlugin::terminate() noexcept {}

Dims NonMaxSuppressionPlugin::getOutputDimensions(int index, const Dims* inputs, int nbInputDims) noexcept
{
    ASSERT(nbInputDims == 2);
    ASSERT(index >= 0 && index < this->getNbOutputs());
    ASSERT(inputs[0].nbDims == 3);
    ASSERT(inputs[1].nbDims == 2 || (inputs[1].nbDims == 3 && inputs[1].d[2] == 1));
    // boxesSize: number of box coordinates for one sample
    boxesSize = inputs[0].d[0] * inputs[0].d[1] * inputs[0].d[2];
    // scoresSize: number of scores for one sample
    scoresSize = inputs[1].d[0] * inputs[1].d[1];
    // num_detections
    if (index == 0)
    {
        Dims dim0{};
        dim0.nbDims = 0;
        return dim0;
    }
    // nmsed_boxes
    if (index == 1)
    {
        return DimsHW(param.keepTopK, 4);
    }
    // nmsed_scores or nmsed_classes
    Dims dim1{};
    dim1.nbDims = 1;
    dim1.d[0] = param.keepTopK;
    return dim1;
}

DimsExprs NonMaxSuppressionDynamicPlugin::getOutputDimensions(
    int outputIndex, const DimsExprs* inputs, int nbInputs, IExprBuilder& exprBuilder) noexcept
{
    ASSERT(nbInputs == 2);
    ASSERT(outputIndex >= 0 && outputIndex < this->getNbOutputs());

    // Shape of boxes input should be
    // Constant shape: [batch_size, num_boxes, num_classes, 4] or [batch_size, num_boxes, 1, 4]
    //           shareLocation ==              0               or          1
    // or
    // Dynamic shape: some dimension values may be -1
    ASSERT(inputs[0].nbDims == 3); // ONNX

    // Shape of scores input should be
    // Constant shape: [batch_size, num_boxes, num_classes] or [batch_size, num_boxes, num_classes, 1]
    // or
    // Dynamic shape: some dimension values may be -1
    ASSERT(inputs[1].nbDims == 3 || inputs[1].nbDims == 4);
    // ONNX
    if (inputs[0].d[0]->isConstant() && inputs[0].d[1]->isConstant() && inputs[0].d[2]->isConstant())
    {
        boxesSize = exprBuilder
                        .operation(DimensionOperation::kPROD,
                            *inputs[0].d[1],
                            *inputs[0].d[2])
                        ->getConstantValue();
    }

    if (inputs[1].d[0]->isConstant() && inputs[1].d[1]->isConstant() && inputs[1].d[2]->isConstant())
    {
        scoresSize
            = exprBuilder.operation(DimensionOperation::kPROD, *inputs[1].d[1], *inputs[1].d[2])->getConstantValue();
    }

    DimsExprs out_dim;
    // nmsedResult
    if (outputIndex == 0)
    {
        out_dim.nbDims = 3;
        // std::cout << "inputs[0].d[0]: " << (inputs[0].d[0])->getConstantValue() << std::endl;
        std::cout << "param.TopK: " << param.topK << std::endl;  
        std::cout << "param.keepTopK: " << param.keepTopK << std::endl;        
        out_dim.d[0] = inputs[0].d[0];
        out_dim.d[1] = exprBuilder.constant(param.keepTopK);
        out_dim.d[2] = exprBuilder.constant(3);
    }

    return out_dim;
}

size_t NonMaxSuppressionPlugin::getWorkspaceSize(int maxBatchSize) const noexcept
{
    return detectionInferenceWorkspaceSize(param.shareLocation, maxBatchSize, boxesSize, scoresSize, param.numClasses,
        numPriors, param.topK, mPrecision, mPrecision);
}

size_t NonMaxSuppressionDynamicPlugin::getWorkspaceSize(
    const PluginTensorDesc* inputs, int nbInputs, const PluginTensorDesc* outputs, int nbOutputs) const noexcept
{
    return detectionInferenceWorkspaceSize(param.shareLocation, inputs[0].dims.d[0], boxesSize, scoresSize,
        param.numClasses, numPriors, param.topK, mPrecision, mPrecision);
}

int NonMaxSuppressionPlugin::enqueue(
    int batchSize, const void* const* inputs, void** outputs, void* workspace, cudaStream_t stream) noexcept
{
    // const void* const locData = inputs[0];
    // const void* const confData = inputs[1];

    // void* keepCount = outputs[0];
    // void* nmsedBoxes = outputs[1];
    // void* nmsedScores = outputs[2];
    // void* nmsedClasses = outputs[3];

    // pluginStatus_t status = nmsInference2(stream, batchSize, boxesSize, scoresSize, param.shareLocation,
    //     param.backgroundLabelId, numPriors, param.numClasses, param.topK, param.keepTopK, param.scoreThreshold,
    //     param.iouThreshold, mPrecision, locData, mPrecision, confData, keepCount, nmsedBoxes, nmsedScores, nmsedClasses,
    //     workspace, param.isNormalized, false, mClipBoxes, mScoreBits);
    // ASSERT(status == STATUS_SUCCESS);
    return 0;
}

int NonMaxSuppressionDynamicPlugin::enqueue(const PluginTensorDesc* inputDesc, const PluginTensorDesc* outputDesc,
    const void* const* inputs, void* const* outputs, void* workspace, cudaStream_t stream) noexcept
{
    const void* const locData = inputs[0];
    const void* const confData = inputs[1];

    void* nmsedResult = outputs[0];

    pluginStatus_t status = nmsInference2(stream, inputDesc[0].dims.d[0], boxesSize, scoresSize, param.shareLocation,
        param.backgroundLabelId, numPriors, param.numClasses, param.topK, param.keepTopK, param.scoreThreshold,
        param.iouThreshold, mPrecision, locData, mPrecision, confData, nmsedResult,
        workspace, param.isNormalized, false, mClipBoxes, mScoreBits);
    ASSERT(status == STATUS_SUCCESS);
    return 0;
}

size_t NonMaxSuppressionPlugin::getSerializationSize() const noexcept
{
    // NMSParameters, boxesSize,scoresSize,numPriors
    return sizeof(NMSParameters) + sizeof(int) * 3 + sizeof(bool) + sizeof(DataType) + sizeof(int32_t);
}

void NonMaxSuppressionPlugin::serialize(void* buffer) const noexcept
{
    char *d = reinterpret_cast<char*>(buffer), *a = d;
    write(d, param);
    write(d, boxesSize);
    write(d, scoresSize);
    write(d, numPriors);
    write(d, mClipBoxes);
    write(d, mPrecision);
    write(d, mScoreBits);
    ASSERT(d == a + getSerializationSize());
}

size_t NonMaxSuppressionDynamicPlugin::getSerializationSize() const noexcept
{
    // NMSParameters, boxesSize,scoresSize,numPriors
    return sizeof(NMSParameters) + sizeof(int) * 3 + sizeof(bool) + sizeof(DataType) + sizeof(int32_t);
}

void NonMaxSuppressionDynamicPlugin::serialize(void* buffer) const noexcept
{
    char *d = reinterpret_cast<char*>(buffer), *a = d;
    write(d, param);
    write(d, boxesSize);
    write(d, scoresSize);
    write(d, numPriors);
    write(d, mClipBoxes);
    write(d, mPrecision);
    write(d, mScoreBits);
    ASSERT(d == a + getSerializationSize());
}

void NonMaxSuppressionPlugin::configurePlugin(const Dims* inputDims, int nbInputs, const Dims* outputDims, int nbOutputs,
    const DataType* inputTypes, const DataType* outputTypes, const bool* inputIsBroadcast,
    const bool* outputIsBroadcast, nvinfer1::PluginFormat format, int maxBatchSize) noexcept
{
    ASSERT(nbInputs == 2);
    ASSERT(nbOutputs == 4);
    ASSERT(inputDims[0].nbDims == 3);
    ASSERT(inputDims[1].nbDims == 2 || (inputDims[1].nbDims == 3 && inputDims[1].d[2] == 1));
    ASSERT(std::none_of(inputIsBroadcast, inputIsBroadcast + nbInputs, [](bool b) { return b; }));
    ASSERT(std::none_of(outputIsBroadcast, outputIsBroadcast + nbInputs, [](bool b) { return b; }));

    boxesSize = inputDims[0].d[0] * inputDims[0].d[1] * inputDims[0].d[2];
    scoresSize = inputDims[1].d[0] * inputDims[1].d[1];
    // num_boxes
    numPriors = inputDims[0].d[0];
    const int numLocClasses = param.shareLocation ? 1 : param.numClasses;
    // Third dimension of boxes must be either 1 or num_classes
    ASSERT(inputDims[0].d[1] == numLocClasses);
    ASSERT(inputDims[0].d[2] == 4);
    mPrecision = inputTypes[0];
}

void NonMaxSuppressionDynamicPlugin::configurePlugin(
    const DynamicPluginTensorDesc* in, int nbInputs, const DynamicPluginTensorDesc* out, int nbOutputs) noexcept
{
    ASSERT(nbInputs == 2);
    ASSERT(nbOutputs == 1);

    const int numLocClasses = param.shareLocation ? 1 : param.numClasses;
    ASSERT(in[0].desc.dims.nbDims == 3);
    ASSERT(in[0].desc.dims.d[2] == 4);

    // Shape of scores input should be
    // Constant shape: [batch_size, num_classes， num_boxes] or [batch_size, num_classes， num_boxes, 1]
    ASSERT(in[1].desc.dims.nbDims == 3 || (in[1].desc.dims.nbDims == 4 && in[1].desc.dims.d[3] == 1));

    boxesSize = in[0].desc.dims.d[1] * in[0].desc.dims.d[2];
    scoresSize = in[1].desc.dims.d[1] * in[1].desc.dims.d[2];
    // num_boxes
    numPriors = in[0].desc.dims.d[1];

    mPrecision = in[0].desc.type;
}

bool NonMaxSuppressionPlugin::supportsFormat(DataType type, PluginFormat format) const noexcept
{
    return ((type == DataType::kHALF || type == DataType::kFLOAT || type == DataType::kINT32)
        && format == PluginFormat::kNCHW);
}

bool NonMaxSuppressionDynamicPlugin::supportsFormatCombination(
    int pos, const PluginTensorDesc* inOut, int nbInputs, int nbOutputs) noexcept
{
    ASSERT(0 <= pos && pos < 3);
    const auto* in = inOut;
    const auto* out = inOut + nbInputs;
    const bool consistentFloatPrecision = in[0].type == in[pos].type;
    switch (pos)
    {
    case 0:
        return (in[0].type == DataType::kHALF || in[0].type == DataType::kFLOAT)
            && in[0].format == PluginFormat::kLINEAR && consistentFloatPrecision;
    case 1:
        return (in[1].type == DataType::kHALF || in[1].type == DataType::kFLOAT)
            && in[1].format == PluginFormat::kLINEAR && consistentFloatPrecision;
    case 2: return out[0].type == DataType::kINT32 && out[0].format == PluginFormat::kLINEAR;
    }
    return false;
}

const char* NonMaxSuppressionPlugin::getPluginType() const noexcept
{
    return NMS_PLUGIN_NAMES[0];
}

const char* NonMaxSuppressionDynamicPlugin::getPluginType() const noexcept
{
    return NMS_PLUGIN_NAMES[1];
}

const char* NonMaxSuppressionPlugin::getPluginVersion() const noexcept
{
    return NMS_PLUGIN_VERSION;
}

const char* NonMaxSuppressionDynamicPlugin::getPluginVersion() const noexcept
{
    return NMS_PLUGIN_VERSION;
}

void NonMaxSuppressionPlugin::destroy() noexcept
{
    delete this;
}

void NonMaxSuppressionDynamicPlugin::destroy() noexcept
{
    delete this;
}

IPluginV2Ext* NonMaxSuppressionPlugin::clone() const noexcept
{
    auto* plugin = new NonMaxSuppressionPlugin(param);
    plugin->boxesSize = boxesSize;
    plugin->scoresSize = scoresSize;
    plugin->numPriors = numPriors;
    plugin->setPluginNamespace(mNamespace.c_str());
    plugin->setClipParam(mClipBoxes);
    plugin->mPrecision = mPrecision;
    plugin->setScoreBits(mScoreBits);
    return plugin;
}

IPluginV2DynamicExt* NonMaxSuppressionDynamicPlugin::clone() const noexcept
{
    auto* plugin = new NonMaxSuppressionDynamicPlugin(param);
    plugin->boxesSize = boxesSize;
    plugin->scoresSize = scoresSize;
    plugin->numPriors = numPriors;
    plugin->setPluginNamespace(mNamespace.c_str());
    plugin->setClipParam(mClipBoxes);
    plugin->mPrecision = mPrecision;
    plugin->setScoreBits(mScoreBits);
    return plugin;
}

void NonMaxSuppressionPlugin::setPluginNamespace(const char* pluginNamespace) noexcept
{
    mNamespace = pluginNamespace;
}

const char* NonMaxSuppressionPlugin::getPluginNamespace() const noexcept
{
    return mNamespace.c_str();
}

void NonMaxSuppressionDynamicPlugin::setPluginNamespace(const char* pluginNamespace) noexcept
{
    mNamespace = pluginNamespace;
}

const char* NonMaxSuppressionDynamicPlugin::getPluginNamespace() const noexcept
{
    return mNamespace.c_str();
}

nvinfer1::DataType NonMaxSuppressionPlugin::getOutputDataType(
    int index, const nvinfer1::DataType* inputTypes, int nbInputs) const noexcept
{
    if (index == 0)
    {
        return nvinfer1::DataType::kINT32;
    }
    return inputTypes[0];
}

nvinfer1::DataType NonMaxSuppressionDynamicPlugin::getOutputDataType(
    int index, const nvinfer1::DataType* inputTypes, int nbInputs) const noexcept
{
    if (index == 0)
    {
        return nvinfer1::DataType::kINT32;
    }
    return inputTypes[0];
}

void NonMaxSuppressionPlugin::setClipParam(bool clip) noexcept
{
    mClipBoxes = clip;
}

void NonMaxSuppressionDynamicPlugin::setClipParam(bool clip) noexcept
{
    mClipBoxes = clip;
}

void NonMaxSuppressionPlugin::setScoreBits(int32_t scoreBits) noexcept
{
    mScoreBits = scoreBits;
}

void NonMaxSuppressionDynamicPlugin::setScoreBits(int32_t scoreBits) noexcept
{
    mScoreBits = scoreBits;
}

bool NonMaxSuppressionPlugin::isOutputBroadcastAcrossBatch(int outputIndex, const bool* inputIsBroadcasted, int nbInputs) const
    noexcept
{
    return false;
}

bool NonMaxSuppressionPlugin::canBroadcastInputAcrossBatch(int inputIndex) const noexcept
{
    return false;
}

NonMaxSuppressionBasePluginCreator::NonMaxSuppressionBasePluginCreator() noexcept
    : params{}
{
    mPluginAttributes.clear();
    mPluginAttributes.emplace_back(PluginField("shareLocation", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("backgroundLabelId", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("numClasses", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("topK", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("keepTopK", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("scoreThreshold", nullptr, PluginFieldType::kFLOAT32, 1));
    mPluginAttributes.emplace_back(PluginField("iouThreshold", nullptr, PluginFieldType::kFLOAT32, 1));
    mPluginAttributes.emplace_back(PluginField("isNormalized", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("clipBoxes", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("scoreBits", nullptr, PluginFieldType::kINT32, 1));
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

NonMaxSuppressionPluginCreator::NonMaxSuppressionPluginCreator() noexcept
{
    mPluginName = NMS_PLUGIN_NAMES[0];
}

NonMaxSuppressionDynamicPluginCreator::NonMaxSuppressionDynamicPluginCreator() noexcept
{
    mPluginName = NMS_PLUGIN_NAMES[1];
}

const char* NonMaxSuppressionBasePluginCreator::getPluginName() const noexcept
{
    return mPluginName.c_str();
}

const char* NonMaxSuppressionBasePluginCreator::getPluginVersion() const noexcept
{
    return NMS_PLUGIN_VERSION;
}

const PluginFieldCollection* NonMaxSuppressionBasePluginCreator::getFieldNames() noexcept
{
    return &mFC;
}

IPluginV2Ext* NonMaxSuppressionPluginCreator::createPlugin(const char* name, const PluginFieldCollection* fc) noexcept
{
    const PluginField* fields = fc->fields;
    mClipBoxes = true;
    mScoreBits = 16;
    for (int i = 0; i < fc->nbFields; ++i)
    {
        const char* attrName = fields[i].name;
        if (!strcmp(attrName, "shareLocation"))
        {
            params.shareLocation = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "backgroundLabelId"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.backgroundLabelId = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "numClasses"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.numClasses = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "topK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.topK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "keepTopK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.keepTopK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "scoreThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.scoreThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "iouThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.iouThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "isNormalized"))
        {
            params.isNormalized = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "clipBoxes"))
        {
            mClipBoxes = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "scoreBits"))
        {
            mScoreBits = *(static_cast<const int32_t*>(fields[i].data));
        }
    }

    auto* plugin = new NonMaxSuppressionPlugin(params);
    plugin->setClipParam(mClipBoxes);
    plugin->setScoreBits(mScoreBits);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

IPluginV2DynamicExt* NonMaxSuppressionDynamicPluginCreator::createPlugin(
    const char* name, const PluginFieldCollection* fc) noexcept
{
    const PluginField* fields = fc->fields;
    mClipBoxes = true;
    mScoreBits = 16;
    for (int i = 0; i < fc->nbFields; ++i)
    {
        const char* attrName = fields[i].name;
        if (!strcmp(attrName, "shareLocation"))
        {
            params.shareLocation = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "backgroundLabelId"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.backgroundLabelId = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "numClasses"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.numClasses = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "topK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.topK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "keepTopK"))
        {
            ASSERT(fields[i].type == PluginFieldType::kINT32);
            params.keepTopK = *(static_cast<const int*>(fields[i].data));
        }
        else if (!strcmp(attrName, "scoreThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.scoreThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "iouThreshold"))
        {
            ASSERT(fields[i].type == PluginFieldType::kFLOAT32);
            params.iouThreshold = *(static_cast<const float*>(fields[i].data));
        }
        else if (!strcmp(attrName, "isNormalized"))
        {
            params.isNormalized = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "clipBoxes"))
        {
            mClipBoxes = *(static_cast<const bool*>(fields[i].data));
        }
        else if (!strcmp(attrName, "scoreBits"))
        {
            mScoreBits = *(static_cast<const int32_t*>(fields[i].data));
        }
    }

    auto* plugin = new NonMaxSuppressionDynamicPlugin(params);
    plugin->setClipParam(mClipBoxes);
    plugin->setScoreBits(mScoreBits);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

IPluginV2Ext* NonMaxSuppressionPluginCreator::deserializePlugin(
    const char* name, const void* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call NMS::destroy()
    auto* plugin = new NonMaxSuppressionPlugin(serialData, serialLength);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

IPluginV2DynamicExt* NonMaxSuppressionDynamicPluginCreator::deserializePlugin(
    const char* name, const void* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call NMS::destroy()
    auto* plugin = new NonMaxSuppressionDynamicPlugin(serialData, serialLength);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

/*
// Copyright (C) 2021-2023 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "models/super_resolution_model.h"

#include <stddef.h>

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <openvino/openvino.hpp>

#include <utils/image_utils.h>
#include <utils/ocv_common.hpp>
#include <utils/slog.hpp>

#include "models/input_data.h"
#include "models/internal_model_data.h"
#include "models/results.h"

SuperResolutionModel::SuperResolutionModel(const std::string& modelFile,
                                           const cv::Size& inputImgSize,
                                           const std::string& layout)
    : ImageModel(modelFile, "standard", false, layout) {
    netInputHeight = inputImgSize.height;
    netInputWidth = inputImgSize.width;
}

void SuperResolutionModel::prepareInputsOutputs(std::shared_ptr<ov::Model>& model) {
    // --------------------------- Configure input & output ---------------------------------------------
    // --------------------------- Prepare input --------------------------------------------------
    const ov::OutputVector& inputs = model->inputs();
    if (inputs.size() != 1 && inputs.size() != 2) {
        throw std::logic_error("Super resolution model wrapper supports topologies with 1 or 2 inputs only");
    }
    std::string lrInputTensorName = inputs.begin()->get_any_name();
    inputNames.push_back(lrInputTensorName);
    ov::Shape lrShape = inputs.begin()->get_shape();
    if (lrShape.size() != 4) {
        throw std::logic_error("Number of dimensions for an input must be 4");
    }
    // in case of 2 inputs they have the same layouts
    ov::Layout inputLayout = getInputLayout(model->inputs().front());

    auto channelsId = ov::layout::channels_idx(inputLayout);
    auto heightId = ov::layout::height_idx(inputLayout);
    auto widthId = ov::layout::width_idx(inputLayout);

    if (lrShape[channelsId] != 1 && lrShape[channelsId] != 3) {
        throw std::logic_error("Input layer is expected to have 1 or 3 channels");
    }

    // A model like single-image-super-resolution-???? may take bicubic interpolation of the input image as the
    // second input
    if (inputs.size() == 2) {
        std::string bicInputTensorName;
        bicInputTensorName = (++inputs.begin())->get_any_name();
        inputNames.push_back(bicInputTensorName);
        ov::Shape bicShape = (++inputs.begin())->get_shape();
        if (bicShape.size() != 4) {
            throw std::logic_error("Number of dimensions for both inputs must be 4");
        }
        if (lrShape[widthId] >= bicShape[widthId] && lrShape[heightId] >= bicShape[heightId]) {
            std::swap(bicShape, lrShape);
            inputNames[0].swap(inputNames[1]);
        } else if (!(lrShape[widthId] <= bicShape[widthId] && lrShape[heightId] <= bicShape[heightId])) {
            throw std::logic_error("Each spatial dimension of one input must surpass or be equal to a spatial"
                                   "dimension of another input");
        }
    }

    ov::preprocess::PrePostProcessor ppp(model);
    for (const auto& input : inputs) {
        inputTransform.setPrecision(ppp, input.get_any_name());
        ppp.input().tensor().set_layout("NHWC");
        ppp.input().model().set_layout(inputLayout);
    }

    // --------------------------- Prepare output -----------------------------------------------------
    const ov::OutputVector& outputs = model->outputs();
    if (outputs.size() != 1) {
        throw std::logic_error("Super resolution model wrapper supports topologies with only 1 output");
    }

    outputNames.push_back(outputs.begin()->get_any_name());
    ppp.output().tensor().set_element_type(ov::element::f32);
    model = ppp.build();

    const ov::Shape& outShape = model->output().get_shape();

    const ov::Layout outputLayout("NCHW");
    const auto outWidth = outShape[ov::layout::width_idx(outputLayout)];
    const auto inWidth = lrShape[ov::layout::width_idx(outputLayout)];
    changeInputSize(model, static_cast<int>(outWidth / inWidth));
}

void SuperResolutionModel::changeInputSize(std::shared_ptr<ov::Model>& model, int coeff) {
    std::map<std::string, ov::PartialShape> shapes;
    const ov::Layout& layout = ov::layout::get_layout(model->inputs().front());
    const auto batchId = ov::layout::batch_idx(layout);
    const auto heightId = ov::layout::height_idx(layout);
    const auto widthId = ov::layout::width_idx(layout);

    const ov::OutputVector& inputs = model->inputs();
    std::string lrInputTensorName = inputs.begin()->get_any_name();
    ov::Shape lrShape = inputs.begin()->get_shape();

    if (inputs.size() == 2) {
        std::string bicInputTensorName = (++inputs.begin())->get_any_name();
        ov::Shape bicShape = (++inputs.begin())->get_shape();
        if (lrShape[heightId] >= bicShape[heightId] && lrShape[widthId] >= bicShape[widthId]) {
            std::swap(bicShape, lrShape);
            std::swap(bicInputTensorName, lrInputTensorName);
        }
        bicShape[batchId] = 1;
        bicShape[heightId] = coeff * netInputHeight;
        bicShape[widthId] = coeff * netInputWidth;
        shapes[bicInputTensorName] = ov::PartialShape(bicShape);
    }

    lrShape[batchId] = 1;
    lrShape[heightId] = netInputHeight;
    lrShape[widthId] = netInputWidth;
    shapes[lrInputTensorName] = ov::PartialShape(lrShape);

    model->reshape(shapes);
}

std::shared_ptr<InternalModelData> SuperResolutionModel::preprocess(const InputData& inputData,
                                                                    InferenceInput& input) {
    auto imgData = inputData.asRef<ImageInputData>();
    auto img = inputTransform(imgData.inputImage);

    auto lrShape = inferenceAdapter->getInputShape(inputNames[0]).get_max_shape();
    const ov::Layout layout("NHWC");

    if (img.channels() != static_cast<int>(lrShape[ov::layout::channels_idx(layout)])) {
        cv::cvtColor(img, img, cv::COLOR_BGR2GRAY);
    }

    if (static_cast<size_t>(img.cols) != netInputWidth || static_cast<size_t>(img.rows) != netInputHeight) {
        slog::warn << "\tChosen model aspect ratio doesn't match image aspect ratio" << slog::endl;
    }
    const size_t height = lrShape[ov::layout::height_idx(layout)];
    const size_t width = lrShape[ov::layout::width_idx(layout)];
    img = resizeImageExt(img, width, height);
    input.emplace(inputNames[0], wrapMat2Tensor(img));

    if (inputNames.size() == 2) {
        auto bicShape = inferenceAdapter->getInputShape(inputNames[1]).get_max_shape();
        const int h = static_cast<int>(bicShape[ov::layout::height_idx(layout)]);
        const int w = static_cast<int>(bicShape[ov::layout::width_idx(layout)]);
        cv::Mat resized;
        cv::resize(img, resized, cv::Size(w, h), 0, 0, cv::INTER_CUBIC);
        input.emplace(inputNames[1], wrapMat2Tensor(resized));
    }

    return std::make_shared<InternalImageModelData>(img.cols, img.rows);
}

std::unique_ptr<ResultBase> SuperResolutionModel::postprocess(InferenceResult& infResult) {
    ImageResult* result = new ImageResult;
    *static_cast<ResultBase*>(result) = static_cast<ResultBase&>(infResult);
    const auto outputData = infResult.getFirstOutputTensor().data<float>();

    std::vector<cv::Mat> imgPlanes;
    const ov::Shape& outShape = infResult.getFirstOutputTensor().get_shape();
    const size_t outChannels = static_cast<int>(outShape[1]);
    const size_t outHeight = static_cast<int>(outShape[2]);
    const size_t outWidth = static_cast<int>(outShape[3]);
    const size_t numOfPixels = outWidth * outHeight;
    if (outChannels == 3) {
        imgPlanes = std::vector<cv::Mat>{cv::Mat(outHeight, outWidth, CV_32FC1, &(outputData[0])),
                                         cv::Mat(outHeight, outWidth, CV_32FC1, &(outputData[numOfPixels])),
                                         cv::Mat(outHeight, outWidth, CV_32FC1, &(outputData[numOfPixels * 2]))};
    } else {
        imgPlanes = std::vector<cv::Mat>{cv::Mat(outHeight, outWidth, CV_32FC1, &(outputData[0]))};
        // Post-processing for text-image-super-resolution models
        cv::threshold(imgPlanes[0], imgPlanes[0], 0.5f, 1.0f, cv::THRESH_BINARY);
    }

    for (auto& img : imgPlanes) {
        img.convertTo(img, CV_8UC1, 255);
    }
    cv::Mat resultImg;
    cv::merge(imgPlanes, resultImg);
    result->resultImage = resultImg;

    return std::unique_ptr<ResultBase>(result);
}

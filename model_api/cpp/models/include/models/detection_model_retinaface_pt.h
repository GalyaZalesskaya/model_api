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

#pragma once
#include <stddef.h>

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core/types.hpp>
#include <utils/nms.hpp>

#include "models/detection_model_ext.h"

namespace ov {
class Model;
class Tensor;
}  // namespace ov
struct InferenceResult;
struct ResultBase;

class ModelRetinaFacePT : public DetectionModelExt {
public:
    struct Box {
        float cX;
        float cY;
        float width;
        float height;
    };

    ModelRetinaFacePT(std::shared_ptr<ov::Model>& model, const ov::AnyMap& configuration);
    ModelRetinaFacePT(std::shared_ptr<InferenceAdapter>& adapter);
    using DetectionModelExt::DetectionModelExt;

    std::unique_ptr<ResultBase> postprocess(InferenceResult& infResult) override;
    static std::string ModelType;

protected:
    size_t landmarksNum = 0;
    float variance[2] = {0.1f, 0.2f};

    enum OutputType { OUT_BOXES, OUT_SCORES, OUT_LANDMARKS, OUT_MAX };

    std::vector<ModelRetinaFacePT::Box> priors;

    std::vector<size_t> filterByScore(const ov::Tensor& scoresTensor, const float confidence_threshold);
    std::vector<float> getFilteredScores(const ov::Tensor& scoresTensor, const std::vector<size_t>& indicies);
    std::vector<cv::Point2f> getFilteredLandmarks(const ov::Tensor& landmarksTensor,
                                                  const std::vector<size_t>& indicies,
                                                  int imgWidth,
                                                  int imgHeight);
    std::vector<ModelRetinaFacePT::Box> generatePriorData();
    std::vector<Anchor> getFilteredProposals(const ov::Tensor& boxesTensor,
                                             const std::vector<size_t>& indicies,
                                             int imgWidth,
                                             int imgHeight);

    void prepareInputsOutputs(std::shared_ptr<ov::Model>& model) override;
    void initDefaultParameters(const ov::AnyMap& configuration);
    void updateModelInfo() override;
};
